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

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
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

// --- Story flags (global variables) -----------------------------------------
// A global, persistent float store: the "your choices matter" backbone. Dialogue
// choices / conditions and schematics read + write these; they persist in the
// .hbsave (serialized alongside objectives + checkpoints). A missing flag reads 0.
void SetFlag(const std::string& name, f32 value);
f32 GetFlag(const std::string& name);
bool HasFlag(const std::string& name);

// --- Inventory (per-playthrough item counts) --------------------------------
// A global item id -> count store (the scavenge/craft backbone), sitting beside
// the story flags: it rides the same .hbsave SerializeState/DeserializeState and
// New-Game Reset lifecycle. Items are free-form string ids (a future .hbitems
// catalog can add display names/icons/caps). `Craft`-style recipes compose from
// HasItem + RemoveItem + AddItem (e.g. a crafting-bench schematic).
void AddItem(const std::string& id, u32 count = 1);
bool RemoveItem(const std::string& id, u32 count = 1); // false if fewer than `count`
u32  ItemCount(const std::string& id);
bool HasItem(const std::string& id, u32 count = 1);
const std::unordered_map<std::string, u32>& Items();
// Selected/equipped weapon (an item id) - the seam a future combat loadout reads.
void EquipWeapon(const std::string& id); // "" = unequip; no-op if not owned
const std::string& EquippedWeapon();

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

// --- Adaptive music (deferred commands) -------------------------------------
// Schematics/scripts drive the score through these without touching the
// AudioSystem; the engine drains the queues each frame into the music director.
void SetMusicState(const std::string& state);          // crossfade to a music state
void SetMusicParameter(const std::string& name, f32 value); // e.g. "intensity"
void PlayStinger(const std::string& asset);            // one-shot `.uaf` (rel. Assets)
// Engine-side drains: the latest pending state, then the parameter + stinger queues.
bool ConsumeMusicState(std::string& outState);
bool ConsumeMusicParameter(std::string& outName, f32& outValue);
bool ConsumeStinger(std::string& outAsset);

// --- Voicelines (deferred one-shots) ----------------------------------------
// Schematics play a spoken line (a `.uaf` Voiceline, rel. Assets); the engine
// drains this each frame and plays it, surfacing its baked "Speaker: caption".
void PlayVoiceline(const std::string& asset);
bool ConsumeVoiceline(std::string& outAsset);

// --- Dialogue (deferred; a `.hbdialogue` sequence) --------------------------
// A schematic starts a conversation; the engine loads it and runs the lines
// over time (each shows its caption + plays its clip). Latest request wins.
void PlayDialogue(const std::string& asset);
bool ConsumeDialogue(std::string& outAsset);
// True if a dialogue/cutscene is queued this frame but not yet consumed. Lets the
// interaction system avoid clobbering a schematic's just-queued conversation (the
// queues are single-slot latest-wins) before the engine's consume block runs.
bool DialoguePending();
bool CutscenePending();

// --- Cutscene (deferred; a `.hbcutscene` timeline) --------------------------
// A schematic starts a cinematic; the engine takes over the camera and runs
// the camera/animation/dialogue tracks over time. Latest request wins.
void PlayCutscene(const std::string& asset);
bool ConsumeCutscene(std::string& outAsset);

// --- UI panel commands (deferred) --------------------------------------------
// Schematic UI nodes drive the panel stack through these; the engine (which owns
// the UIManager) drains the queue each frame, in order (Push then Pop matters).
struct UICommand {
    enum class Op : u8 { Show, Push, Pop };
    Op op = Op::Show;
    std::string panel; // UIPanel.name (unused for Pop)
};
void QueueUICommand(UICommand cmd);
bool ConsumeUICommand(UICommand& out);

// --- Combat deaths (deferred) -----------------------------------------------
// combat::Update flags a kill; the engine drains this each frame and fans it out
// to the schematic OnDeath event (like a UI event to every listener). `tag` is the
// Health.deathTag (or the entity Name); `entity`/`instigator` are entt bits.
struct DeathRec {
    std::string tag;
    u32 entity = 0xFFFFFFFFu;
    u32 instigator = 0xFFFFFFFFu;
};
void QueueDeath(const DeathRec& rec);
bool ConsumeDeath(DeathRec& out); // FIFO

// --- AI noise bus (hearing) -------------------------------------------------
// A world-space sound the AI can hear (footstep, gunshot, thrown prop). `loudness`
// scales a listener's hearing radius; the noise lingers a short TTL so every AI
// agent (which tick at one call site) observes it regardless of frame ordering.
// Multi-listener: Noises() returns ALL live noises; not persisted.
struct Noise {
    glm::vec3 pos{0.0f};
    f32 loudness = 1.0f;
    f32 ttl = 0.0f;
};
void EmitNoise(const glm::vec3& pos, f32 loudness = 1.0f);
const std::vector<Noise>& Noises();
void TickNoises(f32 dt); // ages + expires noises; call once per frame

// --- AI "spotted the player" event (deferred) -------------------------------
// ai::Update flags the rising edge where an agent's awareness first crosses its
// detect threshold; the engine drains this into the schematic OnSpotPlayer event.
struct SpottedRec {
    u32 spotter = 0xFFFFFFFFu; // the AI entity
    u32 target = 0xFFFFFFFFu;  // who it spotted
};
void QueueSpotted(const SpottedRec& rec);
bool ConsumeSpotted(SpottedRec& out);

// Clears the transient per-frame gameplay event queues (deaths / noises / spotted)
// WITHOUT touching the persistent run state. Call on a mid-play level switch so an
// event queued right before the switch can't fire into the freshly-loaded level.
void ClearTransientQueues();

// --- Run-state save/load (objectives + checkpoints) -------------------------
// The SCENE snapshot is saved by the engine; this is just the gameplay bookkeeping.
std::string SerializeState();
void DeserializeState(const std::string& json);
void Reset(); // new game: clear objectives + reached checkpoints + pending save

} // namespace game
} // namespace hbe
