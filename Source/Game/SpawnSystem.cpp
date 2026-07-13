// Game/SpawnSystem.cpp - runtime spawning + encounter management.
#include "Game/SpawnSystem.h"

#include "Core/Log.h"
#include "Game/CombatSystem.h"
#include "Game/GameSystems.h"
#include "Project/Project.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbe::spawn {

namespace {

// Parsed + staged prefab, cached process-wide (parse+stage is the expensive disk
// half; scene::Instantiate keeps its own GPU caches so repeated spawns are cheap).
struct Prefab {
    scene::SceneData data;
    scene::StagedAssets staged;
    bool ok = false;
};
std::unordered_map<std::string, Prefab> g_prefabs;

Prefab* GetPrefab(const std::string& rel) {
    if (rel.empty() || !Project::HasActive()) return nullptr;
    if (auto it = g_prefabs.find(rel); it != g_prefabs.end())
        return it->second.ok ? &it->second : nullptr;
    Prefab pf;
    const std::filesystem::path path = Project::Active().AssetsDir() / rel;
    if (scene::ParseSceneFile(path, pf.data)) {
        scene::StageAssets(pf.data, Project::Active().AssetsDir(), pf.staged);
        pf.ok = true;
    } else {
        HBE_WARN("Spawner: failed to load prefab '{}'.", rel);
    }
    auto [ins, _] = g_prefabs.emplace(rel, std::move(pf));
    return ins->second.ok ? &ins->second : nullptr;
}

bool IsAliveMember(const entt::registry& reg, entt::entity e) {
    const Health* h = reg.try_get<Health>(e);
    return !h || h->alive; // no Health = counts as alive until destroyed
}

glm::vec3 WorldPos(const Scene& scene, entt::entity e) {
    return glm::vec3(scene.WorldMatrix(e)[3]);
}

bool PlayerPos(const Scene& scene, glm::vec3& out) {
    const entt::registry& reg = scene.Registry();
    for (auto e : reg.view<CharacterController>()) {
        out = WorldPos(scene, e);
        return true;
    }
    return false;
}

bool PlayerInBox(const Scene& scene, entt::entity spawner, const glm::vec3& half) {
    glm::vec3 p;
    if (!PlayerPos(scene, p)) return false;
    const glm::vec3 d = glm::abs(p - WorldPos(scene, spawner));
    return d.x <= half.x && d.y <= half.y && d.z <= half.z;
}

// Deterministic golden-angle disc scatter (no RNG, so replays are stable).
glm::vec3 DiscOffset(u32 i, u32 count, f32 radius) {
    if (count <= 1 || radius <= 0.0f) return glm::vec3(0.0f);
    const f32 a = static_cast<f32>(i) * 2.399963f;
    const f32 r = radius * std::sqrt((static_cast<f32>(i) + 0.5f) / static_cast<f32>(count));
    return glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
}

u32 CountAliveSpawner(const entt::registry& reg, const std::string& id) {
    u32 n = 0;
    for (auto e : reg.view<Spawned>())
        if (reg.get<Spawned>(e).spawnerId == id && IsAliveMember(reg, e)) ++n;
    return n;
}

// Destroys every spawned root that matches `spawnerId` plus its whole subtree.
void DespawnBySpawner(Scene& scene, const std::string& spawnerId) {
    entt::registry& reg = scene.Registry();
    std::unordered_set<u32> kill;
    for (auto e : reg.view<Spawned>())
        if (reg.get<Spawned>(e).spawnerId == spawnerId) kill.insert(static_cast<u32>(e));
    if (kill.empty()) return;
    bool grew = true;
    while (grew) {
        grew = false;
        for (auto e : reg.view<Parent>()) {
            if (kill.count(static_cast<u32>(e))) continue;
            if (kill.count(static_cast<u32>(reg.get<Parent>(e).entity))) {
                kill.insert(static_cast<u32>(e));
                grew = true;
            }
        }
    }
    for (u32 bits : kill) {
        const entt::entity e = static_cast<entt::entity>(bits);
        if (reg.valid(e)) reg.destroy(e);
    }
}

// Instantiates `want` copies of the spawner's prefab, scattered on the disc.
void DoBurst(Scene& scene, Renderer& renderer, entt::entity spawnerEnt, Spawner& s, u32 want) {
    if (want == 0) return;
    Prefab* pf = GetPrefab(s.prefab);
    if (!pf) return;
    entt::registry& reg = scene.Registry();
    const glm::vec3 base = WorldPos(scene, spawnerEnt);
    for (u32 i = 0; i < want; ++i) {
        std::vector<entt::entity> created;
        scene::Instantiate(scene, renderer, pf->data, pf->staged, scene::LoadMode::Additive,
                           &created);
        if (created.empty()) continue;
        const entt::entity root = created.front();
        if (Transform* t = reg.try_get<Transform>(root)) t->position = base + DiscOffset(i, want, s.radius);
        reg.emplace_or_replace<Spawned>(root, Spawned{s.encounterId, s.spawnerId});
        ++s.spawnedTotal;
    }
}

void FireClearedAction(const Encounter& en) {
    switch (en.clearedAction) {
        case InteractAction::Dialogue:
            if (!en.clearedAsset.empty()) game::PlayDialogue(en.clearedAsset);
            break;
        case InteractAction::Cutscene:
            if (!en.clearedAsset.empty()) game::PlayCutscene(en.clearedAsset);
            break;
        case InteractAction::SetFlag:
            if (!en.clearedFlag.empty()) game::SetFlag(en.clearedFlag, en.clearedFlagValue);
            break;
        case InteractAction::SetObjective:
            if (!en.clearedFlag.empty()) game::SetObjective(en.clearedFlag, en.clearedText);
            break;
        case InteractAction::CompleteObjective:
            if (!en.clearedFlag.empty()) game::CompleteObjective(en.clearedFlag);
            break;
        case InteractAction::None:
            break;
    }
}

} // namespace

void Update(Scene& scene, Renderer& renderer, f32 dt) {
    entt::registry& reg = scene.Registry();
    auto sv = reg.view<Spawner>();
    if (sv.begin() == sv.end()) return; // no spawners -> zero per-frame cost

    // Snapshot the spawner set first (DoBurst creates entities). Reused buffer: no
    // per-frame heap allocation (matters in Debug).
    static std::vector<entt::entity> spawners;
    spawners.clear();
    for (auto e : sv) spawners.push_back(e);

    for (entt::entity e : spawners) {
        if (!reg.valid(e) || !reg.all_of<Spawner>(e)) continue;
        Spawner& s = reg.get<Spawner>(e);
        s.respawnCooldown = std::max(0.0f, s.respawnCooldown - dt);

        if (s.despawnRequested) {
            DespawnBySpawner(scene, s.spawnerId);
            s.despawnRequested = false;
        }

        const bool available =
            s.requiredFlag.empty() || game::GetFlag(s.requiredFlag) != 0.0f;

        bool burst = false;
        if (s.spawnRequested) {
            burst = true;
            s.spawnRequested = false; // manual / schematic request always fires
        } else if (available) {
            switch (s.trigger) {
                case Spawner::Trigger::Volume: {
                    const bool inside = PlayerInBox(scene, e, s.halfExtents);
                    if (inside && !s.inside && !s.activated) burst = true; // enter-edge, once
                    s.inside = inside;
                    break;
                }
                case Spawner::Trigger::Flag:
                    if (!s.activated && game::GetFlag(s.requiredFlag) != 0.0f) burst = true;
                    break;
                case Spawner::Trigger::Manual:
                    break; // only via spawnRequested
            }
        }

        // Continuous refill toward maxAlive.
        u32 want = s.count;
        if (!burst && s.activated && s.respawn == Spawner::Respawn::Continuous &&
            s.respawnCooldown <= 0.0f && s.maxAlive > 0) {
            const u32 alive = CountAliveSpawner(reg, s.spawnerId);
            if (alive < s.maxAlive) {
                burst = true;
                want = s.maxAlive - alive;
            }
        }

        if (!burst) continue;

        // Throttle the burst to the alive cap.
        if (s.maxAlive > 0) {
            const u32 alive = CountAliveSpawner(reg, s.spawnerId);
            want = alive >= s.maxAlive ? 0u : std::min(want, s.maxAlive - alive);
        }
        DoBurst(scene, renderer, e, s, want);
        s.activated = true;
        s.respawnCooldown = s.respawnDelay;
    }
}

void UpdateEncounters(Scene& scene, f32 dt) {
    (void)dt;
    entt::registry& reg = scene.Registry();
    auto ev = reg.view<Encounter>();
    if (ev.begin() == ev.end()) return; // no encounters -> zero per-frame cost

    // Tally alive members per encounter id from the Spawned tags (reused buffer).
    static std::unordered_map<std::string, u32> alive;
    alive.clear();
    for (auto e : reg.view<Spawned>()) {
        const Spawned& sp = reg.get<Spawned>(e);
        if (!sp.encounterId.empty() && IsAliveMember(reg, e)) ++alive[sp.encounterId];
    }

    for (auto e : ev) {
        Encounter& en = reg.get<Encounter>(e);
        en.clearedEdge = false;
        const bool available =
            en.requiredFlag.empty() || game::GetFlag(en.requiredFlag) != 0.0f;
        en.aliveCount = alive.count(en.id) ? alive[en.id] : 0u;

        // Arm on start, on a schematic/flag request, or as soon as members appear.
        if (en.state == Encounter::State::Idle && available &&
            (en.startActive || en.activateRequested || en.aliveCount > 0)) {
            en.state = Encounter::State::Active;
        }
        en.activateRequested = false;

        if (en.state == Encounter::State::Active) {
            if (en.aliveCount > 0) en.everHadAlive = true;
            else if (en.everHadAlive) {
                en.state = Encounter::State::Cleared;
                en.clearedEdge = true;
                FireClearedAction(en);
            }
        }
    }
}

void ClearPrefabCache() { g_prefabs.clear(); }

} // namespace hbe::spawn
