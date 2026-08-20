// Game/SpawnSystem.cpp - runtime spawning + encounter management.
#include "Game/SpawnSystem.h"

#include "Core/Log.h"
#include "Game/CombatSystem.h"
#include "Game/GameSystems.h"
#include "Project/Project.h"
#include "Scene/Components.h"
#include "Scene/EffectAsset.h" // particle::LoadEffect (.hbvfx)
#include "Scene/Hierarchy.h" // scene::BuildChildrenMap (one parent->children pass)
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp> // glm::quat_cast (world-matrix -> Transform rotation)

#include <cmath>
#include <optional>
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
        // A PREFAB CARRYING UI IS REJECTED, ONCE, LOUDLY. The editor refuses to
        // write one (Editor::CreatePrefabFromSelection), but a hand-edited or
        // pre-migration `.hbprefab` can still exist on disk - and spawning it would
        // create UI entities with no UIDocMember, i.e. loose UI that carries none of
        // BuildSceneJson's skip tags and therefore lands in the next `.hbsave` as
        // permanent, un-clearable screen UI. Exactly the failure the
        // DialogueChoiceButton skip was added to prevent, arriving by another door.
        // UI lives in a `.hbui`; a document IS the reusable UI template.
        bool hasUI = false;
        for (const scene::EntityData& d : pf.data.entities)
            if (d.hasUI || d.hasUICanvas || d.hasUIAnimator || d.hasUIPanel ||
                d.hasUILayoutGroup || d.hasUICanvasGroup) {
                hasUI = true;
                break;
            }
        if (hasUI) {
            HBE_ERROR("Spawner: prefab '{}' contains UI components and will NOT be "
                      "spawned. UI belongs in a .hbui document, not in a .hbprefab.",
                      rel);
        } else {
            scene::StageAssets(pf.data, Project::Active().AssetsDir(), pf.staged);
            pf.ok = true;
        }
    } else {
        HBE_WARN("Spawner: failed to load prefab '{}'.", rel);
    }
    auto [ins, _] = g_prefabs.emplace(rel, std::move(pf));
    return ins->second.ok ? &ins->second : nullptr;
}

// Parsed `.hbvfx` effects, cached process-wide (disk parse is the expensive half; the emitter is
// tiny and copied per spawn). std::nullopt caches a failed/absent asset so it isn't retried on a hot
// spawn path. Cleared on project switch / re-save via ClearPrefabCache.
std::unordered_map<std::string, std::optional<ParticleEmitter>> g_effectCache;

const ParticleEmitter* GetEffect(const std::string& name) {
    if (name.empty() || !Project::HasActive()) return nullptr;
    std::string rel = name;
    if (rel.size() < 6 || rel.compare(rel.size() - 6, 6, ".hbvfx") != 0) rel += ".hbvfx";
    if (auto it = g_effectCache.find(rel); it != g_effectCache.end())
        return it->second ? &*it->second : nullptr;
    std::optional<ParticleEmitter> e =
        particle::LoadEffect(Project::Active().AssetsDir() / rel);
    if (!e) HBE_WARN("SpawnEffect: failed to load effect '{}'.", rel);
    auto [ins, _] = g_effectCache.emplace(rel, std::move(e));
    return ins->second ? &*ins->second : nullptr;
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
    // Descend from each spawned root through ONE parent->children map. This used to
    // be a `while (grew)` fixpoint over the whole Parent pool - worse than quadratic
    // (one full pool scan per level of depth, every despawn), and the worst-shaped
    // walk in the tree. See Scene/Hierarchy.h.
    {
        const scene::ChildrenMap kids = scene::BuildChildrenMap(reg);
        const std::vector<u32> roots(kill.begin(), kill.end());
        for (const u32 bits : roots)
            for (const entt::entity e :
                 scene::SubtreeInOrder(kids, static_cast<entt::entity>(bits)))
                kill.insert(static_cast<u32>(e));
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
        // `base` is the spawner's WORLD position, so the sum is a world point.
        // Instantiate drops the prefab root at the scene root today (local == world),
        // but writing it through the world setter keeps that from becoming a silent
        // misplacement the day a prefab root arrives parented.
        scene.SetWorldPosition(root, base + DiscOffset(i, want, s.radius));
        reg.emplace_or_replace<Spawned>(root, Spawned{s.encounterId, s.spawnerId});
        // A spawned NPC INHERITS its spawner's streaming-shard membership, so despawning
        // that shard takes it too. Without this the NPC is created long after the shard
        // loaded and belongs to nothing: stream::CollectShardEntities' spawnerId rule
        // would still catch it, but only for a spawner with a non-empty spawnerId, and a
        // stranded NPC standing in an unloaded region is a leak that grows per burst.
        if (const StreamShard* sh = reg.try_get<StreamShard>(spawnerEnt))
            reg.emplace_or_replace<StreamShard>(root, *sh);
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

entt::entity SpawnEffect(Scene& scene, const std::string& name, const glm::mat4& transform) {
    const ParticleEmitter* asset = GetEffect(name);
    if (!asset) return entt::null;
    entt::registry& reg = scene.Registry();
    const entt::entity e = scene.CreateEntity("FX:" + name);

    // Decompose the world matrix into the Transform's T/R/S (honours rotation + scale).
    Transform tf;
    const glm::vec3 c0(transform[0]), c1(transform[1]), c2(transform[2]);
    tf.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    const glm::mat3 rot(c0 / glm::max(tf.scale.x, 1e-6f), c1 / glm::max(tf.scale.y, 1e-6f),
                        c2 / glm::max(tf.scale.z, 1e-6f));
    tf.rotation = glm::quat_cast(rot);
    tf.position = glm::vec3(transform[3]);
    reg.emplace<Transform>(e, tf);
    reg.emplace<ParticleEmitter>(e, *asset);

    // A one-shot (non-looping) effect self-destructs once it has finished emitting AND its last
    // particles have died. Continuous emission runs for `duration`; then particles live out their
    // (variance-extended) lifetime. Looping effects persist until explicitly removed.
    if (!asset->loop) {
        const f32 maxLife = asset->lifetime * (1.0f + glm::clamp(asset->lifetimeVariance, 0.0f, 1.0f));
        reg.emplace<EffectLifetime>(e, EffectLifetime{asset->duration + maxLife + 0.5f});
    }
    return e;
}

void Update(Scene& scene, Renderer& renderer, f32 dt) {
    entt::registry& reg = scene.Registry();

    // Drain deferred game::SpawnEffect requests (gameplay/schematics enqueue; we own the Scene).
    for (game::EffectReq req; game::ConsumeEffect(req);) SpawnEffect(scene, req.name, req.transform);

    // Age one-shot effects; destroy those whose particles have all died.
    {
        std::vector<entt::entity> expired;
        for (auto e : reg.view<EffectLifetime>()) {
            EffectLifetime& fx = reg.get<EffectLifetime>(e);
            fx.remaining -= dt;
            if (fx.remaining <= 0.0f) expired.push_back(e);
        }
        for (entt::entity e : expired)
            if (reg.valid(e)) reg.destroy(e);
    }

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
            if (en.aliveCount > 0) {
                en.everHadAlive = true;
                // The population is live again (the shard came back and its spawner
                // re-burst), so absence stops being ambiguous.
                en.membersStreamedOut = false;
            } else if (en.everHadAlive && !en.membersStreamedOut) {
                en.state = Encounter::State::Cleared;
                en.clearedEdge = true;
                FireClearedAction(en);
            }
            // else: alive == 0 because a shard unloaded the members. NOT cleared - the
            // player walked away from the fight. stream::DespawnShard set the flag and
            // re-armed the spawner, so re-entering the area repopulates it (decision 4:
            // spawner progress persists, individual survivors' health resets).
        }
    }
}

void ClearPrefabCache() {
    g_prefabs.clear();
    g_effectCache.clear();
}

} // namespace hbe::spawn
