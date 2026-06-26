// Game/GameSystems.h - run-state gameplay: objectives (task goals) + checkpoints.
//
// TLOU-style: a Checkpoint is reached either by the player walking into its box
// trigger or programmatically (game::ReachCheckpoint from a Schematic/script,
// e.g. an enemy-death graph). Reaching one can set/complete an OBJECTIVE (the
// on-screen task goal) and request a SAVE (the engine, which owns the Scene,
// performs the actual write). State here is the whole run's: it persists across
// level loads and is serialized into the save file.
#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace hbe {

class Scene;

namespace game {

struct Objective {
    std::string id;
    std::string text;   // shown on the HUD via the {objective} token
    bool done = false;
};

// --- Objectives (the task-goal tracker) -------------------------------------
// Adds (or re-activates + retexts) an objective.
void SetObjective(const std::string& id, const std::string& text);
void CompleteObjective(const std::string& id);
const std::vector<Objective>& Objectives();
// The first not-yet-done objective's text (what the HUD shows); "" if none.
std::string CurrentObjectiveText();

// --- Checkpoints ------------------------------------------------------------
// Marks `id` reached (idempotent). When `requestSave`, flags a save the engine
// performs next frame. Safe from Schematics/scripts and the trigger system.
void ReachCheckpoint(const std::string& id, bool requestSave = true);
bool CheckpointReached(const std::string& id);
// True once if a save was requested since the last call (outId = checkpoint id).
bool ConsumeSaveRequest(std::string& outId);

// Per-frame: fires Checkpoint box-triggers the player entered (ReachCheckpoint +
// applies their objective). Call while the simulation is playing.
void UpdateCheckpoints(Scene& scene);

// --- Run-state save/load (objectives + checkpoints) -------------------------
// The SCENE snapshot is saved by the engine; this is just the gameplay bookkeeping.
std::string SerializeState();
void DeserializeState(const std::string& json);
void Reset(); // new game: clear objectives + reached checkpoints + pending save

} // namespace game
} // namespace hbe
