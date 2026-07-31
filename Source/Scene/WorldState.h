// Scene/WorldState.h - persistent per-area, per-SHARD world state across revisits.
//
// Story flags (game::SetFlag) are GLOBAL: fine for "did the player make choice X",
// useless for "is that specific door still open in the warehouse". When a shard is
// streamed out and back in - or the player leaves an area and returns - the scene
// file re-instantiates in its AUTHORED state: every looted crate is full again,
// every killed guard is back, every opened door is shut. This module is the missing
// layer: a compact record of what the player actually changed, captured on the way
// out and re-applied on the way back in.
//
// TWO THINGS CHANGED HERE FOR TAG STREAMING (plan blocker B5), and both are
// load-bearing:
//
// 1. THE KEY IS THE STABLE GUID (Components.h `struct Guid`), not the entity Name.
//    Names are not identity - Scene.h documents that duplicates resolve to "one
//    arbitrary match" - so two same-named crates in one level shared a single
//    persistence row and overwrote each other, and an unnamed door persisted
//    nothing at all. Guids are minted once by Scene::CreateEntity, adopted through
//    guid::Claim on load, and survive despawn/respawn, so they are the only correct
//    key for per-entity state. (The `.hbscene` files of an existing project must be
//    guid-stamped first - see scene::MigrateSceneGuids and why it is time-sensitive.)
//
// 2. THE PAYLOAD IS A JSON BLOB, not five bools. The old EntityState carried
//    interacted/triggered/health/alive and nothing else, so destructible break
//    progress, AI state, spawner progress and encounter progress were all silently
//    lost. CaptureEntityState writes every runtime field that a player can change,
//    in the same shape the scene serializer's own `runtimeTags` blocks use.
//
// SCOPING. State is stored per (area, SHARD KEY) rather than per area. The
// destroyed-entity set is derived by DIFFING the authored member set against what
// was still alive at capture - so it MUST be scoped to the shard being captured.
// An area-wide diff would record every entity of a non-resident shard as
// "destroyed" and then kill it on the next visit. kResidentKey is the reserved key
// for the always-resident set (untagged entities + alwaysLoaded tags), which is
// what a non-streamed level uses for everything.
//
// What persists, keyed by guid:
//   * DESTROYED entities   - looted pickups, consumed props. Derived by diffing, so
//                            nothing has to hook destruction.
//   * Interactable::fired  - a one-shot door/lever stays used.
//   * TriggerVolume::fired - a one-shot trigger does not re-fire on revisit.
//   * Health               - current / alive / deathDispatched: a wounded NPC is
//                            still wounded, a corpse stays a corpse and does not
//                            re-run its one-shot death reactions.
//   * Destructible         - activated + per-chunk state and hp: a half-collapsed
//                            wall is still half-collapsed.
//   * AIBehavior           - FSM state, patrol progress, startAlerted-consumed.
//   * AIPerception         - awareness meter + time since seen.
//   * Spawner              - SPAWNER PROGRESS ONLY (see below).
//   * Encounter            - state + everHadAlive: a cleared camp stays cleared.
//   * Checkpoint::reached  - runtime cache (game:: holds the authoritative set).
//   * Area variables       - free-form floats for scripting ("alarmRaised").
//   * Visit count          - "first time here?" as a one-call query.
//
// SPAWNER-CREATED NPCs ARE DELIBERATELY NOT PERSISTED. Persistable() refuses any
// entity carrying `Spawned`, so a despawn/respawn cycle keeps the SPAWNER's
// progress (activated, spawnedTotal, respawnCooldown) and the ENCOUNTER's state
// (Cleared stays Cleared) while the individual survivors reset. A cleared camp
// stays cleared; a wounded survivor comes back whole. That is what keeps the save
// BOUNDED - persisting a whole spawned population would grow the file with every
// NPC a continuous spawner ever emitted.
//
// Deliberately NOT persisted, and an author has to know it: transforms of arbitrary
// props (physics settles them differently anyway), AI pathing progress, particle
// state, animation phase, in-flight dialogue, and Weapon ammo. Adding any of them
// is a new key in CaptureEntityState / ApplyEntityState and nothing else.
//
// The state rides the .hbsave alongside story flags (game::SerializeState), so a
// reload restores the world exactly as the player left it.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbe {

class Scene;

namespace world {

// The reserved shard key for the ALWAYS-RESIDENT set: untagged entities plus every
// entity of an alwaysLoaded tag. A level with no streaming tags keeps everything
// under this one key, which is why a non-streamed project behaves exactly as it did
// before shards existed.
inline constexpr const char* kResidentKey = "*";

// One shard's persisted delta within an area.
struct ShardState {
    bool captured = false; // `present` is meaningful (a shard can be visited but never captured)
    // Guids still alive when this shard was captured. Anything the freshly
    // instantiated shard has that is NOT in here was destroyed during the visit.
    std::unordered_set<u64> present;
    // guid -> the runtime-state JSON blob (CaptureEntityState output). Only rows
    // that actually carry a delta are stored.
    std::unordered_map<u64, std::string> blobs;
};

struct AreaState {
    u32 visits = 0; // incremented by EnterArea, once per area entry (never per shard)
    std::unordered_map<std::string, ShardState> shards; // key: "<tag>#<index>" or kResidentKey
    // Area-WIDE, deliberately not per shard: the schematic SetAreaVar / GetAreaVar
    // nodes have no shard context and must keep working unchanged.
    std::unordered_map<std::string, f32> vars;
};

class State {
public:
    bool Visited(const std::string& area) const;
    u32 VisitCount(const std::string& area) const;

    void SetVar(const std::string& area, const std::string& name, f32 value);
    f32 GetVar(const std::string& area, const std::string& name) const;

    AreaState& Area(const std::string& area); // creates on demand
    const AreaState* Find(const std::string& area) const;
    // The shard row, or nullptr when the area or the shard was never captured.
    const ShardState* FindShard(const std::string& area, const std::string& shardKey) const;

    void Clear(); // new game

    // FORMAT VERSION 2 (guid keys + JSON blobs). Version 1 (name keys + five bools)
    // still PARSES, but its per-entity rows cannot be migrated - a name does not
    // determine a guid - so they are dropped with a counted warning while `visits`
    // and `vars` (which are not entity-keyed) are preserved. Story progression
    // survives an engine upgrade; individual door/kill deltas from a v1 save do not.
    std::string Serialize() const;
    void Deserialize(const std::string& json);

private:
    std::unordered_map<std::string, AreaState> areas_;
};

// The run's world state (rides .hbsave with the story flags).
State& Get();

// The area currently loaded. Set by EnterArea on every level bind; lets script
// nodes pass an empty area string to mean "here".
void SetCurrentArea(const std::string& area);
const std::string& CurrentArea();
// `area` when non-empty, else CurrentArea().
const std::string& ResolveArea(const std::string& area);

// Canonical area id for a scene path: the filename stem, lower-cased (so
// "Levels/Warehouse.hbscene" and "warehouse" agree).
std::string AreaIdFromPath(const std::filesystem::path& p);

// --- The per-entity blob ------------------------------------------------------
// True when `e` takes part in world state at all: it has a NON-ZERO GUID (the key)
// and at least one component whose runtime state a player can change. Purely
// decorative geometry is skipped, which keeps a capture small.
//
// It is FALSE for anything carrying `Spawned`: spawner-created NPCs are covered by
// their spawner's progress, never individually (see the header note).
bool Persistable(const entt::registry& reg, entt::entity e);

// The runtime state of one entity as a JSON object string, or "" when it carries
// no delta worth replaying. Only RUNTIME fields are written - never authored data -
// so a blob cannot overwrite content and stays small.
std::string CaptureEntityState(const entt::registry& reg, entt::entity e);
// Replays a CaptureEntityState blob onto `e`. Unknown keys and absent components
// are ignored; a malformed blob is a warning, never a crash.
void ApplyEntityState(entt::registry& reg, entt::entity e, const std::string& blob);

// --- Area / shard lifecycle ---------------------------------------------------
// Makes `area` current and bumps its visit count. Call ONCE per area entry, before
// any RestoreGroup: visits count area entries, not shard spawns.
void EnterArea(const std::string& area);

// Records the runtime deltas of exactly `members` under (area, shardKey). Call
// immediately BEFORE destroying them. No-op for an empty area id.
//
// It REFUSES to overwrite a previously captured non-empty row with an empty one:
// capturing a shard whose members are already gone would silently strip the area,
// turning "I killed everything here" into "this shard was never touched".
void CaptureGroup(const Scene& scene, const std::string& area, const std::string& shardKey,
                  const std::vector<entt::entity>& members);

// Re-applies a previously captured (area, shardKey) onto the freshly instantiated
// `members`. Call immediately AFTER instantiating them, same frame. Anything in
// `members` whose guid is absent from the captured `present` set is destroyed
// (subtree included) - it was looted/consumed during the visit. Does NOT bump the
// visit count; EnterArea owns that.
void RestoreGroup(Scene& scene, const std::string& area, const std::string& shardKey,
                  const std::vector<entt::entity>& members);

// --- Whole-scene convenience (a level with no streaming) ----------------------
// CaptureArea/RestoreArea are CaptureGroup/RestoreGroup over the ALWAYS-RESIDENT
// set: every persistable entity that is NOT a streamed shard member (i.e. carries
// no StreamShard). Scoping them that way is what stops a level with a despawned
// shard from recording that shard's absent entities as destroyed.
//
// RestoreArea also calls EnterArea, so a plain level load is still one call.
void CaptureArea(const Scene& scene, const std::string& area);
void RestoreArea(Scene& scene, const std::string& area);

} // namespace world
} // namespace hbe
