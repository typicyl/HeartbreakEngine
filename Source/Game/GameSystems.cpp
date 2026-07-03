// Game/GameSystems.cpp - objectives + checkpoints implementation.
#include "Game/GameSystems.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <unordered_set>
#include <utility>
#include <vector>

namespace hbe::game {

namespace {
std::vector<Objective> g_objectives;
std::unordered_set<std::string> g_reached;
bool g_saveRequested = false;
std::string g_saveId;
// Deferred adaptive-music commands (drained by the engine).
bool g_musicStatePending = false;
std::string g_musicState;
std::vector<std::pair<std::string, f32>> g_musicParams;
std::vector<std::string> g_stingers;
std::vector<std::string> g_voicelines; // one-shot voiceline clips (rel. Assets)
std::string g_dialogue;                // latest requested `.hbdialogue` (rel. Assets)
bool g_dialoguePending = false;
std::string g_cutscene;                // latest requested `.hbcutscene` (rel. Assets)
bool g_cutscenePending = false;
// Deferred UI panel commands (drained by the engine, in order).
std::vector<UICommand> g_uiCommands;
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

void QueueUICommand(UICommand cmd) { g_uiCommands.push_back(std::move(cmd)); }
bool ConsumeUICommand(UICommand& out) {
    if (g_uiCommands.empty()) return false;
    out = std::move(g_uiCommands.front());
    g_uiCommands.erase(g_uiCommands.begin());
    return true;
}

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

std::string SerializeState() {
    nlohmann::json j;
    nlohmann::json& objs = j["objectives"] = nlohmann::json::array();
    for (const Objective& o : g_objectives)
        objs.push_back({{"id", o.id}, {"text", o.text}, {"done", o.done}});
    nlohmann::json& cps = j["checkpoints"] = nlohmann::json::array();
    for (const std::string& id : g_reached) cps.push_back(id);
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
    } catch (const std::exception& e) {
        HBE_WARN("game: failed to parse save state: {}", e.what());
    }
}

void Reset() {
    g_objectives.clear();
    g_reached.clear();
    g_saveRequested = false;
    g_saveId.clear();
    g_musicStatePending = false;
    g_musicState.clear();
    g_musicParams.clear();
    g_stingers.clear();
}

} // namespace hbe::game
