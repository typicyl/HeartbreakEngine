// Game/GameSystems.cpp - objectives + checkpoints implementation.
#include "Game/GameSystems.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/WorldState.h" // per-area revisit state rides the same save blob

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hbe::game {

namespace {
std::vector<Objective> g_objectives;
std::unordered_set<std::string> g_reached;
std::unordered_map<std::string, f32> g_flags; // global story variables (persisted)
std::unordered_map<std::string, u32> g_items; // inventory item id -> count (persisted)
std::string g_equippedWeapon;                 // equipped item id ("" = none)
bool g_saveRequested = false;
std::string g_saveId;
// Deferred adaptive-music commands (drained by the engine).
bool g_musicStatePending = false;
std::string g_musicState;
std::vector<std::pair<std::string, f32>> g_musicParams;
std::vector<std::string> g_stingers;
std::vector<std::string> g_voicelines; // one-shot voiceline clips (rel. Assets)
std::vector<SubtitleReq> g_subtitles;  // audio-less subtitle lines (cutscene narration)
f32 g_cameraShake = 0.0f;              // accumulated impulse trauma this frame
std::string g_dialogue;                // latest requested `.hbdialogue` (rel. Assets)
bool g_dialoguePending = false;
std::string g_cutscene;                // latest requested `.hbcutscene` (rel. Assets)
bool g_cutscenePending = false;
std::string g_sequence;                // latest requested `.hbseq` (rel. Assets)
bool g_sequencePending = false;
// Deferred UI panel commands (drained by the engine, in order).
std::vector<UICommand> g_uiCommands;
std::vector<DeathRec> g_deaths; // combat deaths, drained into the OnDeath event
std::vector<Noise> g_noises;    // live AI-audible sounds (TTL-aged)
std::vector<SpottedRec> g_spotted; // AI "spotted" edges, drained into OnSpotPlayer
} // namespace

void SetMusicState(const std::string& state) {
    g_musicState = state;
    g_musicStatePending = true;
}
void SetMusicParameter(const std::string& name, f32 value) {
    g_musicParams.emplace_back(name, value);
}
void PlayStinger(const std::string& asset) { g_stingers.push_back(asset); }

bool ConsumeMusicState(std::string& outState) {
    if (!g_musicStatePending) return false;
    g_musicStatePending = false;
    outState = g_musicState;
    return true;
}
bool ConsumeMusicParameter(std::string& outName, f32& outValue) {
    if (g_musicParams.empty()) return false;
    outName = g_musicParams.front().first;
    outValue = g_musicParams.front().second;
    g_musicParams.erase(g_musicParams.begin());
    return true;
}
bool ConsumeStinger(std::string& outAsset) {
    if (g_stingers.empty()) return false;
    outAsset = g_stingers.front();
    g_stingers.erase(g_stingers.begin());
    return true;
}
void PlayVoiceline(const std::string& asset) { g_voicelines.push_back(asset); }
bool ConsumeVoiceline(std::string& outAsset) {
    if (g_voicelines.empty()) return false;
    outAsset = g_voicelines.front();
    g_voicelines.erase(g_voicelines.begin());
    return true;
}
void QueueSubtitle(const std::string& speaker, const std::string& text, f32 duration) {
    if (text.empty()) return;
    g_subtitles.push_back({speaker, text, duration});
}
bool ConsumeSubtitle(SubtitleReq& out) {
    if (g_subtitles.empty()) return false;
    out = std::move(g_subtitles.front());
    g_subtitles.erase(g_subtitles.begin());
    return true;
}
void QueueCameraShake(f32 trauma) {
    // Accumulate within the frame; the rig clamps on apply. Several impacts in
    // one frame should add up, not overwrite each other.
    g_cameraShake += glm::max(trauma, 0.0f);
}
bool ConsumeCameraShake(f32& outTrauma) {
    if (g_cameraShake <= 0.0f) return false;
    outTrauma = g_cameraShake;
    g_cameraShake = 0.0f;
    return true;
}
void PlayDialogue(const std::string& asset) {
    g_dialogue = asset;
    g_dialoguePending = true;
}
bool ConsumeDialogue(std::string& outAsset) {
    if (!g_dialoguePending) return false;
    g_dialoguePending = false;
    outAsset = g_dialogue;
    return true;
}
bool DialoguePending() { return g_dialoguePending; }
bool CutscenePending() { return g_cutscenePending; }
void PlayCutscene(const std::string& asset) {
    g_cutscene = asset;
    g_cutscenePending = true;
}
bool ConsumeCutscene(std::string& outAsset) {
    if (!g_cutscenePending) return false;
    g_cutscenePending = false;
    outAsset = g_cutscene;
    return true;
}
bool SequencePending() { return g_sequencePending; }
void PlaySequence(const std::string& asset) {
    g_sequence = asset;
    g_sequencePending = true;
}
bool ConsumeSequence(std::string& outAsset) {
    if (!g_sequencePending) return false;
    g_sequencePending = false;
    outAsset = g_sequence;
    return true;
}

void QueueUICommand(UICommand cmd) { g_uiCommands.push_back(std::move(cmd)); }
bool ConsumeUICommand(UICommand& out) {
    if (g_uiCommands.empty()) return false;
    out = std::move(g_uiCommands.front());
    g_uiCommands.erase(g_uiCommands.begin());
    return true;
}

void QueueDeath(const DeathRec& rec) { g_deaths.push_back(rec); }
bool ConsumeDeath(DeathRec& out) {
    if (g_deaths.empty()) return false;
    out = std::move(g_deaths.front());
    g_deaths.erase(g_deaths.begin());
    return true;
}

void EmitNoise(const glm::vec3& pos, f32 loudness) {
    Noise n;
    n.pos = pos;
    n.loudness = std::max(0.0f, loudness);
    n.ttl = 0.3f; // linger a few frames so every AGENT observes it
    g_noises.push_back(n);
}
const std::vector<Noise>& Noises() { return g_noises; }
void TickNoises(f32 dt) {
    for (Noise& n : g_noises) n.ttl -= dt;
    g_noises.erase(std::remove_if(g_noises.begin(), g_noises.end(),
                                  [](const Noise& n) { return n.ttl <= 0.0f; }),
                   g_noises.end());
}

void QueueSpotted(const SpottedRec& rec) { g_spotted.push_back(rec); }
bool ConsumeSpotted(SpottedRec& out) {
    if (g_spotted.empty()) return false;
    out = std::move(g_spotted.front());
    g_spotted.erase(g_spotted.begin());
    return true;
}

void ClearTransientQueues() {
    g_deaths.clear();
    g_noises.clear();
    g_spotted.clear();
    // A shake or narration line queued right before a level switch must not fire
    // into the freshly loaded world.
    g_subtitles.clear();
    g_cameraShake = 0.0f;
}

void SetFlag(const std::string& name, f32 value) {
    if (name.empty()) return;
    // Guard non-finite values: NaN/Inf can't be represented in JSON (dump() emits
    // null) and would corrupt the flag on the next save round-trip.
    g_flags[name] = std::isfinite(value) ? value : 0.0f;
}
f32 GetFlag(const std::string& name) {
    const auto it = g_flags.find(name);
    return it != g_flags.end() ? it->second : 0.0f;
}
bool HasFlag(const std::string& name) { return g_flags.count(name) != 0; }

void SetObjective(const std::string& id, const std::string& text) {
    for (Objective& o : g_objectives)
        if (o.id == id) {
            o.text = text;
            o.done = false;
            return;
        }
    g_objectives.push_back({id, text, false});
    HBE_INFO("[Objective] {} - {}", id, text);
}

void CompleteObjective(const std::string& id) {
    for (Objective& o : g_objectives)
        if (o.id == id) {
            if (!o.done) HBE_INFO("[Objective] completed: {}", id);
            o.done = true;
            return;
        }
}

const std::vector<Objective>& Objectives() { return g_objectives; }

std::string CurrentObjectiveText() {
    for (const Objective& o : g_objectives)
        if (!o.done) return o.text;
    return {};
}

void ReachCheckpoint(const std::string& id, bool requestSave) {
    if (id.empty()) return;
    if (!g_reached.insert(id).second) return; // already reached
    HBE_INFO("[Checkpoint] reached: {}", id);
    if (requestSave) {
        g_saveRequested = true;
        g_saveId = id;
    }
}

bool CheckpointReached(const std::string& id) { return g_reached.count(id) != 0; }

bool ConsumeSaveRequest(std::string& outId) {
    if (!g_saveRequested) return false;
    g_saveRequested = false;
    outId = g_saveId;
    return true;
}

void UpdateCheckpoints(Scene& scene) {
    entt::registry& reg = scene.Registry();
    // Player = the (first) CharacterController entity.
    auto players = reg.view<Transform, CharacterController>();
    if (players.begin() == players.end()) return;
    const glm::vec3 player = glm::vec3(scene.WorldMatrix(*players.begin())[3]);

    for (const entt::entity e : reg.view<Transform, Checkpoint>()) {
        Checkpoint& cp = reg.get<Checkpoint>(e);
        if (!cp.triggerOnEnter || cp.id.empty()) continue;
        if (cp.once && CheckpointReached(cp.id)) {
            cp.reached = true;
            continue;
        }
        const glm::vec3 c = glm::vec3(scene.WorldMatrix(e)[3]);
        const glm::vec3 d = glm::abs(player - c);
        if (d.x <= cp.halfExtents.x && d.y <= cp.halfExtents.y && d.z <= cp.halfExtents.z) {
            ReachCheckpoint(cp.id, cp.saveOnReach);
            cp.reached = true;
            if (!cp.setObjective.empty()) SetObjective(cp.id, cp.setObjective);
            if (!cp.completesObjective.empty()) CompleteObjective(cp.completesObjective);
        }
    }
}

void AddItem(const std::string& id, u32 count) {
    if (id.empty() || count == 0) return;
    g_items[id] += count;
}
bool RemoveItem(const std::string& id, u32 count) {
    auto it = g_items.find(id);
    if (it == g_items.end() || it->second < count) return false;
    it->second -= count;
    if (it->second == 0) {
        g_items.erase(it); // keep the map clean (like flags)
        if (g_equippedWeapon == id) g_equippedWeapon.clear(); // no longer owned
    }
    return true;
}
u32 ItemCount(const std::string& id) {
    auto it = g_items.find(id);
    return it != g_items.end() ? it->second : 0u;
}
bool HasItem(const std::string& id, u32 count) { return ItemCount(id) >= count; }
const std::unordered_map<std::string, u32>& Items() { return g_items; }
void EquipWeapon(const std::string& id) {
    if (id.empty() || g_items.count(id) > 0) g_equippedWeapon = id; // must own it
}
const std::string& EquippedWeapon() { return g_equippedWeapon; }

std::string SerializeState() {
    nlohmann::json j;
    nlohmann::json& objs = j["objectives"] = nlohmann::json::array();
    for (const Objective& o : g_objectives)
        objs.push_back({{"id", o.id}, {"text", o.text}, {"done", o.done}});
    nlohmann::json& cps = j["checkpoints"] = nlohmann::json::array();
    for (const std::string& id : g_reached) cps.push_back(id);
    nlohmann::json& fl = j["flags"] = nlohmann::json::object();
    for (const auto& [name, value] : g_flags) fl[name] = value;
    nlohmann::json& items = j["items"] = nlohmann::json::object();
    for (const auto& [id, n] : g_items) items[id] = n;
    j["equipped"] = g_equippedWeapon;
    // Per-area world state (what the player changed in each area) rides the same
    // save blob as the story flags, so a reload restores revisited areas exactly.
    j["world"] = world::Get().Serialize();
    return j.dump();
}

void DeserializeState(const std::string& json) {
    Reset();
    if (json.empty()) return;
    try {
        const nlohmann::json j = nlohmann::json::parse(json);
        for (const nlohmann::json& o : j.value("objectives", nlohmann::json::array()))
            g_objectives.push_back(
                {o.value("id", std::string()), o.value("text", std::string()), o.value("done", false)});
        for (const nlohmann::json& c : j.value("checkpoints", nlohmann::json::array()))
            g_reached.insert(c.get<std::string>());
        if (const auto fit = j.find("flags"); fit != j.end() && fit->is_object())
            for (const auto& [name, value] : fit->items())
                if (value.is_number()) g_flags[name] = value.get<f32>(); // skip null/string (corrupt)
        if (const auto iit = j.find("items"); iit != j.end() && iit->is_object())
            for (const auto& [id, value] : iit->items())
                if (value.is_number_unsigned()) g_items[id] = value.get<u32>();
        g_equippedWeapon = j.value("equipped", std::string());
        world::Get().Deserialize(j.value("world", std::string()));
    } catch (const std::exception& e) {
        HBE_WARN("game: failed to parse save state: {}", e.what());
    }
}

void Reset() {
    g_objectives.clear();
    g_reached.clear();
    g_flags.clear();
    world::Get().Clear(); // new game: every area is unvisited again
    g_items.clear();
    g_equippedWeapon.clear();
    g_saveRequested = false;
    g_saveId.clear();
    g_musicStatePending = false;
    g_musicState.clear();
    g_musicParams.clear();
    g_stingers.clear();
    // Newer deferred queues (parity with the music queues): clear so a request
    // queued right before a restart/level transition can't fire into the fresh run.
    g_voicelines.clear();
    g_subtitles.clear();
    g_cameraShake = 0.0f;
    g_dialogue.clear();
    g_dialoguePending = false;
    g_cutscene.clear();
    g_cutscenePending = false;
    g_sequence.clear();
    g_sequencePending = false;
    g_uiCommands.clear();
    g_deaths.clear();
    g_noises.clear();
    g_spotted.clear();
}

} // namespace hbe::game
