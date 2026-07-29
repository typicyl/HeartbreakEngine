// Scene/WorldState.h - persistent per-AREA world state across revisits.
//
// Story flags (game::SetFlag) are GLOBAL: fine for "did the player make choice X",
// useless for "is that specific door still open in the warehouse". When the player
// leaves an area and comes back, the level file reloads from disk in its AUTHORED
// state - every looted crate is full again, every killed guard is back, every
// opened door is shut. This module is the missing layer: a compact per-area record
// of what the player actually changed, captured on the way out and re-applied on
// the way back in.
//
// What persists (keyed by entity Name, which is stable across loads - entt handles
// are not):
//   * DESTROYED entities   - looted pickups, killed NPCs, consumed props. Derived
//                            by diffing the authored set against what was still
//                            alive at capture, so nothing has to hook destruction.
//   * Interactable::fired  - a one-shot door/lever stays used.
//   * TriggerVolume::fired - a one-shot trigger does not re-fire on revisit.
//   * Health               - a wounded-but-alive NPC is still wounded.
//   * Area variables       - free-form floats for scripting ("alarmRaised").
//   * Visit count          - "first time here?" is a one-call query, which is what
//                            most revisit scripting actually wants.
//
// The state rides the .hbsave alongside story flags (game::SerializeState), so a
// reload restores the world exactly as the player left it.
//
// Deliberately NOT persisted: transforms of arbitrary props (physics settles them
// differently anyway) and AI positions. Both are extension points - see
// CaptureArea if you want an opt-in component for them.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hbe {

class Scene;

namespace world {

// One entity's persisted delta within an area.
struct EntityState {
    bool interacted = false; // Interactable::fired
    bool triggered = false;  // TriggerVolume::fired
    bool hasHealth = false;
    f32 health = 0.0f;
    bool alive = true;
};

struct AreaState {
    u32 visits = 0; // incremented by RestoreArea
    // Names still present when the area was captured. Anything the freshly loaded
    // area has that is NOT in here was destroyed during the visit.
    std::unordered_set<std::string> present;
    bool captured = false; // `present` is meaningful (an area can be visited but never captured)
    std::unordered_map<std::string, EntityState> entities;
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

    void Clear(); // new game

    std::string Serialize() const;
    void Deserialize(const std::string& json);

private:
    std::unordered_map<std::string, AreaState> areas_;
};

// The run's world state (rides .hbsave with the story flags).
State& Get();

// The area currently loaded. Set by the engine on every level load; lets script
// nodes pass an empty area string to mean "here".
void SetCurrentArea(const std::string& area);
const std::string& CurrentArea();
// `area` when non-empty, else CurrentArea().
const std::string& ResolveArea(const std::string& area);

// Records the current scene's gameplay deltas under `area`. Call immediately
// BEFORE unloading/replacing the area. No-op for an empty area id.
void CaptureArea(const Scene& scene, const std::string& area);

// Re-applies a previously captured `area` onto the freshly loaded scene and bumps
// its visit count. Call immediately AFTER the area finishes loading. On a first
// visit this only bumps the count.
void RestoreArea(Scene& scene, const std::string& area);

// Canonical area id for a level base / scene path: the filename stem, lower-cased
// (so "Levels/Warehouse.hbscene" and "warehouse" agree).
std::string AreaIdFromPath(const std::filesystem::path& p);

} // namespace world
} // namespace hbe
