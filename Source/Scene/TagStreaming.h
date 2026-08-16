// Scene/TagStreaming.h - the RUNTIME half of tag streaming: bind a level, then
// spawn and despawn its baked shards, automatically, by distance.
//
// WHAT DRIVES IT. Streamer::Update takes the streaming FOCI (the player and the
// active camera - see FocusPoints) and asks stream::Evaluate (Scene/StreamPolicy.h)
// which shards should load and which should go. The policy is a separate, pure file
// so its five rules are testable without a world; everything with a side effect -
// jobs, the registry, the GPU, persistence - is here.
//
// THE THREE COSTS, AND WHAT BOUNDS EACH. This engine is GPU-bound in Release with
// about 0.4 ms of CPU headroom at 6000 objects, so "evaluate everything every frame"
// was never available:
//
//   * EVALUATION is O(shards x foci) - a handful of float ops per shard, no registry
//     touch at all - and runs on a CADENCE (kEvalFrameInterval frames elapsed OR a
//     focus moved kEvalMoveDist metres), not every frame. It is O(SHARDS), never
//     O(entities): that is what the save-time bake buys.
//   * STAGING (parse-free asset load) is the expensive half and runs on the JOB
//     SYSTEM, at most PolicyIn::maxConcurrent at a time. scene::StageAssets is
//     documented thread-safe (no registry, no GPU).
//   * FINALIZING is scene::Instantiate: main thread only, and BUDGETED TO ONE SHARD
//     PER FRAME (salvage::FinalizeBudget - SALVAGE 3, whose "~1-2 s streaming jank"
//     is a MEASURED number from this engine's own cell streamer, not an estimate).
//     Several shards becoming ready in the same frame - the normal case when a
//     loading screen ends or the player turns a corner - would otherwise stack their
//     instantiate cost into one frame. Despawns are capped the same way, for the same
//     reason: a despawn is a synchronous capture + closure + destroy.
//
// A FAILED SHARD IS TERMINAL, and that is SALVAGE 3's own correction to the code it
// preserves. The original reset Failed to Unloaded and warned; because the shard was
// still inside its load radius, the same Update re-entered Loading, so a durably
// broken shard retried EVERY FRAME while the player stood near it - re-parsing into a
// reused, never-cleared scratch buffer, so each retry appended to the last attempt's
// rows. Here a failure clears the shard's staged scratch, warns ONCE, and stays
// Failed. IsSettled deliberately ignores Failed (a broken shard would otherwise hang
// the loading screen forever).
//
// WHAT A DESPAWN ACTUALLY RECLAIMS: entities, draw items, physics bodies (Jolt reaps
// them lazily on its next Update), spatial audio voices (same), nav obstacles (rebuilt
// from live views), particle pools, and CPU component memory.
//
// IT RECLAIMS ZERO VRAM. There is no mesh or texture destroy anywhere in the RHI -
// the only destroy is DestroyGpuBuffer, for compute buffers - and the instantiate
// caches are process-wide and monotonic. Tag streaming is a CPU / draw-call /
// simulation feature. Do not describe it as a memory win, here or in a log line.
//
// AND ONE CASE STILL GROWS IT. Everything a respawn would otherwise re-upload is
// cached across cycles: GPU meshes and textures (InstantiateCaches), morph atlases
// (blocker B3), surface-paint canvases, and world-space UI render targets. Chunked
// TERRAIN is the exception - terrain::Sync calls renderer.UploadMesh per chunk with no
// cache key, so a TerrainComponent inside a streamed tag adds a whole chunk set per
// cycle that nothing can free. The shard bake warns about exactly that configuration
// (tagshard::Bake's terrain validator) and terrain is a monolithic world floor no tag
// should be streaming anyway, so it is a diagnosed authoring error rather than a leak
// on the normal path - but the honest statement is "reclaims none, and this one adds".
//
// THREE INVARIANTS THIS FILE EXISTS TO HOLD:
//
// 1. THE ENVIRONMENT IS BOUND ONCE, BY THE LEVEL, NOT BY A SHARD. LoadMode::Replace
//    is the only place ambientIntensity/exposure/shadowDistance/post/giSource are
//    applied and the only place the old world is destroyed, and Replace is illegal
//    for a slice. BindLevel calls scene::BindWorld for exactly that, so N shard
//    spawns leave the same environment behind that one full load would have.
//
// 2. DESPAWN IS A CLOSURE OVER LIVE STATE, NEVER A REPLAY OF THE LOAD LIST.
//    scene::Instantiate creates entities AFTER filling its createdOut (modular
//    character parts), and terrain chunks, destruction debris, world-UI surfaces and
//    spawned NPCs all appear later still. Replaying the load list leaks every one of
//    them. CollectShardEntities re-derives membership from the registry.
//
// 3. NO HANDLE OUTLIVES ITS ENTITY. Despawn scrubs every raw entt::entity field in
//    SURVIVING components that pointed into the set it is about to destroy
//    (Health::lastAttacker, AIPerception::knownTarget, Destructible::chunkEntity[],
//    DebrisChunk::owner, Character::liveParts, SkinnedPartRef::character,
//    UICanvas::surface, UISurface::canvas, and a Parent whose target dies). A
//    reference that cannot be scrubbed - a schematic Entity pin, a game::DeathRec
//    instigator, both raw u32 bits - is validated at its READ site instead, by
//    schematic::BakedEnt / BakedGet (SchematicSystem.h), the one place the interpreter
//    and the transpiled C++ both resolve a pin. That guard lives in the Schematic layer
//    because Schematic sits BELOW Scene and cannot depend on this file;
//    --test-shardstate section 7 pins it there.
#pragma once

#include "Scene/SceneSerializer.h"
#include "Scene/StreamPolicy.h"
#include "Scene/TagShard.h"
// NOT StreamingSalvage.h: it includes nlohmann/json.hpp for SALVAGE 1's template, and
// nlohmann is linked PRIVATE to the engine libraries - so a public header that pulls it
// in breaks every executable's own translation unit (main_editor.cpp, main_runtime.cpp).
// The salvaged pieces are used in the .cpp instead: the atomic below holds
// salvage::RegionState values, and the one-finalize-per-frame budget is a local in
// Update (a FinalizeBudget constructed fresh each frame IS "at most one per frame",
// which is why it needs no member).

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

class Scene;
class Renderer;

namespace stream {

// Evaluate at most every N frames, or as soon as a focus has moved this far. Both
// halves matter: the frame count bounds the cost of standing still, the distance
// bounds how stale the decision can get while sprinting. At 8 m/s a 2 m threshold
// fires every ~4 frames at 16 ms anyway, so in motion the distance rule dominates
// and the frame rule is what makes an idle player free.
inline constexpr u32 kEvalFrameInterval = 4;
inline constexpr f32 kEvalMoveDist = 2.0f;
// Loads in flight at once. Bounds worker IO and, more importantly, how many shards
// can pile up waiting for the one-per-frame finalize.
inline constexpr u32 kMaxConcurrentLoads = 4;

// Where an entity lives, answerable WITHOUT the entity existing - which is the whole
// point: a cross-shard reference has to be able to tell "gone forever" from "not
// loaded right now".
enum class Residency {
    NotInLevel,   // no entity of that name/guid in the bound level at all
    Resident,     // in a streamed shard that is currently spawned
    StreamedOut,  // in a streamed shard that is not spawned (it WILL come back)
    AlwaysLoaded, // untagged, or in an alwaysLoaded tag: never streamed
};

const char* ToString(Residency r);

// One shard's runtime record.
struct ShardRuntime {
    scene::ShardDesc desc;
    std::vector<u32> rows;  // entity rows in the source file (the slice)
    // Streaming config, resolved once at bind from the shard's TagDef (so the policy
    // never has to look a tag up per evaluation).
    f32 loadRadius = 0.0f;
    f32 unloadRadius = 0.0f;
    i32 priority = 0;
    bool resident = false;
    // Entities scene::Instantiate reported for the last spawn. DIAGNOSTIC ONLY -
    // never the despawn source of truth (invariant 2 above).
    std::vector<entt::entity> lastCreated;

    // --- Async load state --------------------------------------------------
    // Published by the staging job with release semantics and read by the main
    // thread with acquire, which is the whole synchronisation between them; values
    // are salvage::RegionState (SALVAGE 4 pins them to the deleted streamer's, value
    // for value, so the static_casts in the .cpp stay honest). 0 == Unloaded.
    std::atomic<int> state{0};
    // The staging job's output, consumed by the main-thread finalize and then FREED -
    // it is a full CPU copy of every mesh/texture the shard references, and holding
    // it for the whole residency would trade the CPU memory a despawn reclaims for
    // CPU memory a spawn never gives back. Re-staging on the next load is cheap: the
    // process-wide mesh cache makes StageAssets skip anything already GPU-resident.
    scene::StagedAssets staged;
    // AUTHORING ONLY (BindMode::AuthorWorld). What this shard looked like IN THE
    // REGISTRY the last time it was despawned, as a scene JSON string - and the source
    // it respawns from instead of the level FILE.
    //
    // The file is the wrong source in the editor, and silently so. `source_` was parsed
    // at bind, so a despawn/respawn cycle reverted every edit the author had made inside
    // that zone since the last save (and the save path itself triggers the respawn, so
    // Ctrl+S then wrote the reverted content). Worse, an entity CREATED inside the zone -
    // Ctrl+D on a window that is a child of a streamed building - is in no file row at
    // all: the despawn's Parent closure destroyed it and nothing could ever bring it
    // back, with no undo entry, because streamed despawns deliberately never PushUndo.
    //
    // Deliberately NOT world:: - an authoring bind writes nothing there (see SpawnRows),
    // and player-progress deltas are the wrong shape for authored edits anyway. Empty
    // until the first authoring despawn, so a shipping runtime never allocates it.
    std::string authorSnapshot;
    // AUTHORING async respawn: the snapshot parsed to SceneData on the STAGING WORKER, so
    // the heavy StageAssets (mip-gen etc.) runs off the main thread exactly like the file
    // path - without this the editor did a synchronous parse+stage+upload on respawn (a
    // ~1.5s load hitch). When set, Finalize instantiates from THIS instead of the sliced
    // source file. Freed by Finalize after use. Null on a shipping (non-authoring) runtime.
    std::unique_ptr<scene::SceneData> snapshotData;
    bool failWarned = false; // a Failed shard warns once, then stays quiet
    // Already counted as a deferred finalize. Without it the counter increments once per
    // Ready shard per missed FRAME, so four shards ready together read as six deferrals
    // for four shards and the number cannot say whether the queue is backing up.
    bool deferred = false;
    class Streamer* owner = nullptr; // stable back-pointer for the job payload
};

// Per-run streaming counters. The honest way to report what streaming cost and what
// it did; --tagstreamtest prints these.
struct StreamStats {
    u32 evaluations = 0;
    u32 spawns = 0;   // finalizes completed
    u32 despawns = 0;
    u32 asyncStages = 0; // staging jobs kicked onto the job system
    u32 syncStages = 0;  // staged inline (no job system, or a manual SpawnShard)
    // SHARDS the one-per-frame budget made wait, counted once each (not once per shard
    // per missed frame - that number could not distinguish four shards waiting one frame
    // from one shard waiting four).
    u32 deferredFinalizes = 0;
    u32 failures = 0;
    u32 residentPeak = 0;
    // Milliseconds. `eval` is the policy pass; `structural` is a finalize or a
    // despawn - the two costs that are budgeted, measured separately because they
    // are bounded by different things.
    //
    // `max*` are SINCE BIND (or since ResetStats); `win*` are since the last
    // ResetWindowStats. The engine's 2-second Perf line prints the WINDOW maxima,
    // because a lifetime maximum makes one 12 ms finalize at level start report
    // "max structural 12.00 ms" forever - useless for spotting a NEW regression while
    // playing. --tagstreamtest reports the lifetime ones, which is what it wants.
    f64 lastEvalMs = 0.0, maxEvalMs = 0.0, totalEvalMs = 0.0, winMaxEvalMs = 0.0;
    f64 lastStructuralMs = 0.0, maxStructuralMs = 0.0, totalStructuralMs = 0.0;
    f64 winMaxStructuralMs = 0.0;
    u32 framesUpdated = 0;
};

// How a bind relates to the world that is already in the registry.
enum class BindMode {
    // The normal path: destroy the previous world, apply this level's environment
    // (scene::BindWorld), enter the area, spawn the always-resident slice.
    Fresh,
    // The `.hbsave` path: the world has ALREADY been instantiated from a snapshot, so
    // bind only the shard table + the area id. No BindWorld (it would destroy the
    // restored world), no visit bump (the player never re-entered), no resident spawn
    // (it is already there - spawning it again is the double-spawn P7 has to avoid).
    // Follow it with AdoptResidency.
    AdoptWorld,
    // A 3D MAIN MENU backdrop. Binds like Fresh - scene::BindWorld (destroy + apply
    // the level's environment) and spawn the always-resident slice, so the world is
    // genuinely THERE behind the menu - but leaves world:: persistence completely
    // alone: no EnterArea (a menu is not a visit, and the AreaVisitCount schematic
    // node's FirstVisit output must still fire on the player's REAL first entry), no
    // SetCurrentArea, and - enforced by the same silence on the way out - no
    // CaptureGroup/CaptureArea when the menu world is torn down for Play. The menu
    // always shows the AUTHORED world, never save-modified state, and leaves zero
    // trace in any save. Distance streaming runs normally against it (the menu
    // camera is a focus), so a tagged menu zone loads exactly as it would in game.
    MenuWorld,
    // THE EDITOR'S path (live zones). The registry already holds the AUTHORED world,
    // loaded by the editor's own scene load, and it must survive: this mode does not
    // UnloadAll, does not call scene::BindWorld (whose first statement is DestroyWorld
    // - the single reason the editor could never stream), does not re-apply the
    // environment, does not spawn the resident slice (it is already standing, and
    // spawning it again would duplicate it with fresh guids because guid::Claim
    // refuses to adopt a guid that is taken), and does not touch world:: persistence
    // at all.
    //
    // WHY NO world:: WRITES. A despawn calls world::CaptureGroup and a respawn calls
    // world::RestoreGroup; both read and write game:: player-progress state that
    // NOTHING reverts while authoring, and RestoreGroup will re-destroy any member
    // that happened to be absent at capture time. Authoring has no progress deltas to
    // preserve, so the whole layer is skipped rather than trusted to a partial net.
    //
    // Follow it with AdoptResidency (with an EMPTY key list): in the editor every
    // entity of the file is already live, so every shard is adopted by guid, nothing
    // is spawned, and the streamer ends up owning exactly what is already there.
    AuthorWorld,
};

// A MANUAL residency override, per shard. Editor-facing: the author says "show me
// this zone" or "get it out of my way" and distance stops deciding.
//
// SESSION-ONLY BY CONSTRUCTION - it lives here, in the streamer, and nowhere in
// TagDef or the `.hbproj`. A persisted override would ship a project whose streaming
// silently differs from the runtime's for a reason invisible in the runtime, and a
// forgotten "force unloaded" surviving a save would look exactly like content loss.
enum class ShardForce : u8 {
    Auto = 0, // distance + associations decide (the runtime rule)
    Resident, // load however far away, never unload - AND drive this tag's associations
    Unloaded, // despawn however close, never load - beats associations and distance
};

// The streaming FOCI: the player's world position (the CharacterController, if there
// is one) AND the active camera position. Both, always: a cutscene camera flies ahead
// of a stationary player, and a camera-zone/spline shot can look somewhere the player
// will never stand. `out` is cleared first.
void FocusPoints(const Scene& scene, const glm::vec3& cameraPos, std::vector<glm::vec3>& out);

// Binds one level file and owns its shards' residency. One per world; the engine
// holds a single instance.
class Streamer {
public:
    Streamer() = default;
    // A staging job holds a raw pointer into shards_; the streamer may not die (nor
    // may Reset free the shards) while one is in flight. That exact use-after-free was
    // live in the deleted cell streamer (plan blocker B13).
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;
    // Parses `sceneFile`, reads its baked shard table, clears + binds the world's
    // environment (scene::BindWorld), enters the area for persistence, and spawns the
    // ALWAYS-RESIDENT slice: every entity that is untagged, in an alwaysLoaded tag, or
    // in no trusted shard. Streamed shards start UNLOADED.
    //
    // An UNTRUSTED shard table (stale header, hand edit - tagshard::FromParsed decides)
    // degrades to "everything always loaded": zero streamed shards, one resident slice
    // containing the whole file. A stale bake must cost streaming, never content.
    //
    // Binding while already bound is a LEVEL TRANSITION: the outgoing level is captured
    // (UnloadAll) before its world is destroyed. A failed parse changes nothing at all.
    //
    // `assetsDir` is where StageAssets resolves meshes/textures/materials from, and
    // `defs` is the project's tag list (it decides which tags are alwaysLoaded). Both
    // are explicit rather than taken from Project::Active() so this is drivable
    // headlessly - the same reason tagshard::Bake takes `defs`.
    bool BindLevel(Scene& scene, Renderer& renderer, const std::filesystem::path& sceneFile,
                   const std::filesystem::path& assetsDir, const std::vector<TagDef>& defs,
                   BindMode mode = BindMode::Fresh);

    // Forgets the binding. Does NOT touch the world and does NOT capture - call
    // UnloadAll first if the world is going away and its state matters. Drains any
    // staging job first, so nothing is left pointing at freed shards.
    //
    // Pass the world it was bound to and its Scene::Streaming() summary is cleared
    // with it. Omitting it is SAFE but conservative: a stale summary makes the
    // editor's save path refuse a world it no longer describes, which is the
    // direction to be wrong in when the alternative is writing a level with holes.
    void Reset(Scene* scene = nullptr);

    // Pushes the current shard residency onto the Scene (see StreamingResidency).
    // Called from every site that changes residency; public so a caller that mutates
    // shards through SpawnShard/DespawnShard in a loop can re-publish once at the end.
    void PublishResidency(Scene& scene) const;

    bool IsBound() const { return bound_; }
    // True for a BindMode::AuthorWorld bind: this streamer is driving the EDITOR's
    // authored world, not a runtime level. Published onto the Scene so the save path
    // can tell the two apart (StreamingResidency::authoring).
    bool Authoring() const { return authoring_; }
    bool Trusted() const { return trusted_; }
    const std::string& UntrustedReason() const { return untrustedReason_; }
    const std::string& AreaId() const { return areaId_; }
    const std::filesystem::path& LevelPath() const { return levelPath_; }

    usize ShardCount() const { return shards_.size(); }
    const ShardRuntime& Shard(u32 i) const { return *shards_[i]; }
    // "<tag>#<index>" - the persistence key and the human-facing name.
    std::string ShardKey(u32 i) const;
    // -1 when no shard has that key.
    i32 FindShard(const std::string& key) const;
    bool IsResident(u32 i) const { return i < shards_.size() && shards_[i]->resident; }
    // RULE 6: is this shard being held resident by an ASSOCIATED tag (rather than by
    // its own distance)? The answer from the last evaluation - it is derived, not
    // stored, so it is only meaningful once one has run. The author-facing "why is
    // this here?" readout.
    bool IsAssociated(u32 i) const {
        return i < shardAssociated_.size() && shardAssociated_[i] != 0u;
    }
    // Is this shard in range ON ITS OWN TERMS (inside its load radius, or still inside
    // its own hysteresis band having been)? The other half of the "why is this here?"
    // readout: `associated` alone can no longer answer it, because a shard can be BOTH
    // driven and in range and the mark is deliberately set in that case (a tag's other
    // shards depend on it). "Held ONLY by an association" is IsAssociated && !IsSelfHeld.
    bool IsSelfHeld(u32 i) const {
        return i < shardSelfSeed_.size() && shardSelfSeed_[i] != 0u;
    }
    // Re-resolves the tag->tag association graph against a new project tag list WITHOUT
    // touching the binding, the shards or any residency. The editor calls this when the
    // author edits an association in the Tags panel: the graph is otherwise resolved
    // exactly once, at BindLevel, so a live-bound streamer would go on using the
    // relations the project had when the scene was last saved and the author's edit
    // would silently do nothing.
    void RefreshAssociations(const std::vector<TagDef>& defs);
    u32 ResidentShardCount() const;
    // Keys of every resident shard, sorted - what a `.hbsave` records.
    std::vector<std::string> ResidentKeys() const;
    // Rows in the always-resident slice (untagged + alwaysLoaded + untrusted fallback).
    usize ResidentRowCount() const { return residentRows_.size(); }

    // --- Automatic distance streaming ----------------------------------------
    // ONE call per frame, main thread. Finalizes at most one ready shard (SALVAGE 3),
    // reaps failures, and - on its cadence - evaluates the policy and kicks/orders the
    // result. Cheap on a frame that does none of those things: an atomic load per
    // shard and nothing else.
    //
    // The caller decides WHEN this may run. In the shipping runtime that is
    // GameState::Playing and GameState::Loading - Loading included deliberately,
    // because the loading screen waits on IsSettled and a streamer that does not run
    // behind the curtain can never settle.
    void Update(Scene& scene, Renderer& renderer, const std::vector<glm::vec3>& foci);

    // --- MANUAL residency override (per shard) --------------------------------
    // Composes with everything else through ONE mechanism - the association seed set -
    // rather than being a second, parallel notion of residency. Precedence, strictly:
    //
    //   1. An alwaysLoaded tag has NO ShardRuntime at all, so there is nothing here to
    //      force. It is already resident forever.
    //   2. Unloaded beats associations and distance: the shard is excluded from the
    //      seed set (it stops DRIVING), its `associated` flag is cleared (it stops
    //      being driven) and it despawns however close a focus is.
    //   3. Resident beats distance: the shard's tag is ADDED to the seed set, so it
    //      also pulls in its own associations - force the hill, get the vista, which
    //      is the useful behaviour - and the shard itself is held exactly the way an
    //      associated shard is held (loads out of range, never unloads).
    //   4. Auto is the runtime rule, unchanged.
    //
    // The backing vector is EMPTY until the first non-Auto call, so a shipping runtime
    // that never touches this pays one `empty()` branch per evaluation.
    void SetShardForce(u32 i, ShardForce f);
    ShardForce ShardForceOf(u32 i) const;
    void ClearShardForces();
    u32 ForcedShardCount() const;

    // ONE shard held resident for as long as the caller says so, on top of whatever
    // the policy decides and WITHOUT reading as a manual override in the UI. The
    // editor points this at the shard owning the current selection: despawning the
    // object the author has selected - possibly is dragging - is the worst failure
    // this feature has, and it must not look like a forced zone afterwards. -1 = none.
    void SetHeldShard(i32 i) { heldShard_ = i; }
    i32 HeldShard() const { return heldShard_; }

    // BRING EVERY SHARD RESIDENT, SYNCHRONOUSLY, RIGHT NOW. Drains any staging job,
    // discards half-finished loads (a Ready shard finalized later would double-spawn
    // what this call already spawned) and then SpawnShards the rest inline.
    //
    // This is what makes a mid-stream SAVE safe: the save path calls it first, so the
    // completeness refusal is never even reached in the normal case. Returns the
    // number of shards STILL not resident - which can only be Failed ones, and which
    // is exactly the count that must keep the save refused.
    u32 SpawnAllShards(Scene& scene, Renderer& renderer);

    // Removes the StreamShard membership stamp from every live entity. The editor
    // calls it when it unbinds, so "nothing is bound" really does mean "nothing in
    // this viewport is streamed" - the stamp is NOT serialized, so a stamp left
    // behind after an unbind is a label with no owner, and the Hierarchy would go on
    // colouring authored objects as streamed content forever.
    void ClearMembership(Scene& scene) const;

    // false PINS EVERYTHING LOADED (the semantics the deleted StreamingWorld::LoadAll
    // had): every shard loads and none unloads. "Streaming off" has to mean "the whole
    // level is here", never "half the level is missing and nothing will fix it".
    void SetEnabled(bool e) { enabled_ = e; }
    bool Enabled() const { return enabled_; }

    // "Is the world the player is about to be shown actually there?" - the LOADING
    // SCREEN's gate. All four salvaged clauses (SALVAGE 4) apply: a shard Loading or
    // Ready is not settled (Ready means the worker finished but the main thread has not
    // instantiated - dropping the screen then is precisely the pop it exists to hide);
    // an in-range Unloaded shard is not settled; and Failed is IGNORED, deliberately,
    // because a broken shard never appears and waiting on it would hang the screen
    // forever.
    bool IsSettled(const std::vector<glm::vec3>& foci) const;

    const StreamStats& Stats() const { return stats_; }
    void ResetStats() { stats_ = StreamStats{}; }
    // Clears only the WINDOW maxima, for a caller that reports per-interval worsts
    // (the engine's Perf line) without destroying the since-bind totals the streaming
    // self-test reports.
    void ResetWindowStats() { stats_.winMaxEvalMs = stats_.winMaxStructuralMs = 0.0; }

    // Instantiates shard `i` as a SLICE of the bound level (never Replace), stamps
    // StreamShard membership, and replays its captured state by guid. False when the
    // index is out of range or the shard is already resident.
    bool SpawnShard(Scene& scene, Renderer& renderer, u32 i);

    // Captures shard `i`'s state, then destroys its live closure. False when the index
    // is out of range or the shard is not resident.
    bool DespawnShard(Scene& scene, u32 i);

    // Despawns every resident shard (capturing each). The level-teardown call.
    void UnloadAll(Scene& scene);

    // Captures every resident shard AND the always-resident set without destroying
    // anything - the "about to write a save" call.
    void CaptureAllLoaded(const Scene& scene);

    // --- Save/load residency (P7) --------------------------------------------
    // Reconciles a just-restored world with this binding. Two jobs, in one pass,
    // because both are "which shards are already standing":
    //
    //   1. ADOPT. Every live entity whose guid belongs to a streamed shard is stamped
    //      with StreamShard and its shard is marked resident. This is what makes a
    //      LEGACY (v1) `.hbsave` - a complete whole-world snapshot, written before
    //      shards existed - streamable for the rest of the session instead of frozen:
    //      without it the streamer would believe nothing is resident and could never
    //      despawn what the snapshot restored.
    //   2. SPAWN THE REST. Any key in `residentKeys` whose shard was NOT adopted is
    //      spawned now (its state comes from the world:: blobs the save carried).
    //      A key that WAS adopted is skipped - that skip is the double-spawn guard.
    //
    // Returns how many shards it spawned. Requires a BindMode::AdoptWorld bind.
    u32 AdoptResidency(Scene& scene, Renderer& renderer,
                       const std::vector<std::string>& residentKeys);

    // Residency of a name / a guid in the BOUND level. Name lookup uses the level
    // FILE's names, so it answers for entities that are not currently instantiated;
    // duplicate names resolve to the first row, matching Scene::FindByName's
    // documented arbitrary-match behaviour.
    Residency QueryName(const std::string& name) const;
    Residency QueryGuid(u64 guid) const;
    // Shard index owning that guid, or -1 (untagged, always-loaded, or not in level).
    i32 ShardOfGuid(u64 guid) const;

private:
    void SpawnRows(Scene& scene, Renderer& renderer, const std::vector<u32>& rows,
                   const std::string& shardKey, i32 shardIndex,
                   std::vector<entt::entity>* createdOut);
    // Main-thread half of an async load: instantiate the staged slice, stamp
    // membership, replay persisted state, free the staged payload.
    void Finalize(Scene& scene, Renderer& renderer, u32 i);
    // AUTHORING respawn from ShardRuntime::authorSnapshot instead of the level file.
    // False when there is no snapshot (or it will not parse), which sends the caller
    // down the normal file path.
    bool SpawnAuthorSnapshot(Scene& scene, Renderer& renderer, u32 i);
    // The staging job body (worker thread). `arg` is a ShardRuntime*.
    static void StageJob(void* arg);
    // Authoring respawn staging (worker thread): parse the shard's authorSnapshot into
    // ShardRuntime::snapshotData and StageAssets it, so the editor's respawn does its
    // mip-gen/decode off the main thread (Finalize then only uploads). `arg` is a
    // ShardRuntime*. Leaves snapshotData null (and clears authorSnapshot) if it will not
    // parse, so Finalize falls back to the file slice.
    static void SnapshotStageJob(void* arg);
    // Blocks until no staging job is in flight. MAIN THREAD ONLY.
    void DrainInFlight() const;
    f32 NearestFocusDistance(u32 shard, const std::vector<glm::vec3>& foci) const;

    bool bound_ = false;
    bool trusted_ = true;
    bool enabled_ = true;
    bool authoring_ = false; // BindMode::AuthorWorld - see the enum
    // BindMode::MenuWorld. Shares authoring_'s world:: silence (no visit, no capture,
    // no restore) WITHOUT its editor behaviours (author snapshots, adopt-in-place).
    // Kept separate because overloading authoring_ would drag those in too.
    bool silent_ = false;
    std::string untrustedReason_;
    std::filesystem::path levelPath_, assetsDir_;
    std::string areaId_;
    scene::SceneData source_;
    // unique_ptr because ShardRuntime holds an atomic (not movable) AND because a
    // staging job holds a raw pointer to one: the address has to survive the vector
    // growing.
    std::vector<std::unique_ptr<ShardRuntime>> shards_;
    std::vector<u32> residentRows_;

    // --- Per-frame machinery -------------------------------------------------
    // (SALVAGE 3's one-finalize-per-frame budget is a LOCAL in Update - see the include
    // note at the top of this file.)
    std::atomic<u32> inFlight_{0};
    u64 frame_ = 0;
    u64 lastEvalFrame_ = 0;
    std::vector<glm::vec3> lastEvalFoci_;
    // Set when an evaluation was throttled (or a bind just happened): re-evaluate next
    // frame instead of waiting out the cadence, so a backlog drains at one item per
    // frame rather than one per cadence period.
    bool forceEval_ = true;
    std::vector<PolicyShard> policyScratch_; // reused; no per-evaluation allocation
    // Per-shard "a live Spawned member of mine is standing next to a focus" flag, so a
    // pursuing NPC is never destroyed in the player's face just because his home
    // shard's BAKED box went out of range. Reused; no per-evaluation allocation.
    std::vector<u8> pinnedByMember_;
    // --- RULE 6: tag association --------------------------------------------
    // The tag->tag graph, resolved ONCE at BindLevel from the project's tag list, so
    // the per-evaluation pass is O(tags + edges) with no string work and no
    // allocation. Per evaluation the streamer fills assoc_.seed from DISTANCE ALONE
    // (plus alwaysLoaded, which always drives) and runs the SHARED propagation -
    // the same stream::AssocPass the editor's Tags-panel prediction runs, so the two
    // cannot disagree. Nothing here persists across an evaluation except the graph.
    stream::AssocPass assoc_;
    bool assocActive_ = false;        // any edge at all; false = the pass is skipped entirely
    std::vector<u32> shardTag_;       // shard -> node in assoc_.graph (kNoAssocTag = none)
    std::vector<u8> alwaysLoadedTag_; // tag -> permanently seeded (see BindLevel)
    // Shard -> "held by a driver", from the LAST evaluation. Read by the fill loop,
    // by IsSettled (an associated shard is out of range by definition, so without
    // this the loading screen would drop before the vista appeared) and by the
    // author-facing readout.
    std::vector<u8> shardAssociated_;
    // Shard -> "in range on its OWN terms", from the last evaluation. This is the seed
    // input to the propagation AND it carries the seed's hysteresis: it is set inside
    // the load radius and only cleared outside the UNLOAD radius, so a focus sitting on
    // a driver's load boundary cannot toggle what that driver pulls in. Deliberately
    // NOT the shard's `resident` flag, which is false for the whole staging window and
    // forever for a Failed shard - see the seed loop in Update.
    std::vector<u8> shardSelfSeed_;
    // Per-shard ShardForce. EMPTY unless something actually forced a shard, so the
    // runtime cost of the whole manual-override feature is one empty() test.
    std::vector<u8> shardForce_;
    i32 heldShard_ = -1; // see SetHeldShard
    PolicyOut policyOut_;
    StreamStats stats_;
    // guid -> shard index (-1 = always resident). Built once at BindLevel from the
    // FILE, so it answers for entities that do not currently exist.
    std::unordered_map<u64, i32> guidToShard_;
    // name -> first file row carrying it.
    std::unordered_map<std::string, u32> nameToRow_;
    std::vector<i32> rowToShard_; // parallel to source_.entities
};

// Every live entity belonging to shard `i`, as a CLOSURE over the registry:
//   (a) StreamShard{i} holders,
//   (b) their transitive Parent closure (the fixed-point loop from
//       spawn::DespawnBySpawner, which is the working precedent),
//   (c) Spawned whose spawnerId names a Spawner in the set,
//   (d) DebrisChunk whose owner is in the set,
//   (e) UISurface whose canvas is in the set (and the reverse link),
//   (f) SkinnedPartRef whose character is in the set, and Character::liveParts.
// Exposed for testing: the leak this prevents is invisible in an entity count taken
// one frame later.
void CollectShardEntities(const Scene& scene, u32 shard, std::vector<entt::entity>& out);

// Headless proof of the whole thing (--test-shardstate): a shard despawned and
// respawned restores door / kill / pickup / trigger / destructible / AI / encounter
// state BY GUID (including two entities that share a name, which the old name key
// collapsed), a spawner's progress survives while its spawned population does not, no
// surviving component holds a dangling handle, a non-resident shard is not diffed as
// destroyed, and two despawn/respawn cycles are stable in both entity count and
// stored-state size.
//
// It also drives AUTOMATIC streaming end to end: a focus swept across the shards
// spawns and despawns them by distance, ONE finalize lands per frame with the rest
// counted as deferred, a focus parked in the dead band between the two radii does not
// unload, disabling pins everything loaded, a stationary focus evaluates on the cadence
// rather than every frame, and the same sweep repeated returns the entity count exactly
// to baseline. Then both `.hbsave` shapes: a v2 snapshot that EXCLUDES shard members
// restores with the shards respawned and no entity duplicated, and a legacy whole-world
// snapshot is ADOPTED so the streamer can manage what it restored. Both the job-system
// path and the synchronous fallback are exercised. No GPU, no window.
bool SelfTest();

// Headless proof of RULE 6, ASSOCIATED TAGS, end to end through a real Streamer
// (--test-assoctags). The authored scenario is the motivating one, by name: three
// SEPARATE tags - City, City_LowPoly and Hill - each with its own content, its own
// baked bounds and its own radii, with Hill associating City_LowPoly. Standing on
// the hill spawns the low-poly city from kilometres away; leaving the hill releases
// it unless it is in range on its own; a shard resident for both reasons survives
// losing either; a cycle terminates instead of pinning the world; an association
// naming a tag that does not exist warns once and streams normally; and the added
// evaluation cost stays inside the streaming budget. Also the `.hbproj` round trip
// and the bake's association diagnostics. No GPU, no window.
//
// It asserts NOTHING about City and City_LowPoly excluding each other, deliberately:
// they are separate assets and may be co-resident. That is the author's business,
// expressed by the radii they set, and not the engine's to prevent.
bool AssocSelfTest();

} // namespace stream
} // namespace hbe
