// Scene/WorldState.cpp
#include "Scene/WorldState.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/EntityGuid.h"
#include "Scene/Hierarchy.h" // scene::ChildrenOf (one parent->children pass)
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <vector>

namespace hbe::world {
namespace {
using json = nlohmann::json;

// Current on-disk format of State::Serialize. 1 = the pre-streaming, Name-keyed,
// five-bool format (parsed for its visits/vars, entity rows dropped).
constexpr u32 kFormatVersion = 2;

State g_state;
std::string g_currentArea;

// Destroys `e` and its whole subtree. A looted pickup or a killed NPC is usually
// a parent with mesh/collider children, so destroying just the root would leave
// the visible part behind. Mirrors Editor::DestroyRecursive.
void DestroySubtree(entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return;
    // ONE pool pass for the whole subtree, then destroy leaf-first. The recursive
    // shape this replaced called scene::ChildrenOf per node - and ChildrenOf scans the
    // WORLD-WIDE Parent pool and sorts it - so it was O(N x pool) with a sort at every
    // node, not the single pass the comment claimed. SubtreeInOrder is pre-order, so
    // reverse iteration is leaf-first; the list is built up front because destroying
    // mutates the pools it is derived from.
    const std::vector<entt::entity> all = scene::SubtreeInOrder(reg, e);
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (reg.valid(*it)) reg.destroy(*it);
}

u64 GuidOf(const entt::registry& reg, entt::entity e) {
    const Guid* g = reg.valid(e) ? reg.try_get<const Guid>(e) : nullptr;
    return g ? g->value : 0;
}

} // namespace

// --- State -------------------------------------------------------------------

bool State::Visited(const std::string& area) const {
    const AreaState* a = Find(area);
    return a && a->visits > 0;
}

u32 State::VisitCount(const std::string& area) const {
    const AreaState* a = Find(area);
    return a ? a->visits : 0u;
}

void State::SetVar(const std::string& area, const std::string& name, f32 value) {
    if (area.empty() || name.empty()) return;
    Area(area).vars[name] = value;
}

f32 State::GetVar(const std::string& area, const std::string& name) const {
    const AreaState* a = Find(area);
    if (!a) return 0.0f;
    const auto it = a->vars.find(name);
    return it != a->vars.end() ? it->second : 0.0f;
}

AreaState& State::Area(const std::string& area) { return areas_[area]; }

const AreaState* State::Find(const std::string& area) const {
    const auto it = areas_.find(area);
    return it != areas_.end() ? &it->second : nullptr;
}

const ShardState* State::FindShard(const std::string& area, const std::string& shardKey) const {
    const AreaState* a = Find(area);
    if (!a) return nullptr;
    const auto it = a->shards.find(shardKey);
    return it != a->shards.end() ? &it->second : nullptr;
}

void State::Clear() { areas_.clear(); }

std::string State::Serialize() const {
    json root = json::object();
    root["version"] = kFormatVersion;
    json jareas = json::object();
    for (const auto& [name, a] : areas_) {
        json ja;
        ja["visits"] = a.visits;
        if (!a.vars.empty()) {
            json vars = json::object();
            for (const auto& [k, v] : a.vars) vars[k] = v;
            ja["vars"] = std::move(vars);
        }
        if (!a.shards.empty()) {
            json jshards = json::object();
            for (const auto& [key, sh] : a.shards) {
                json js;
                js["captured"] = sh.captured;
                if (sh.captured) {
                    json present = json::array();
                    for (const u64 g : sh.present) present.push_back(guid::ToHex(g));
                    js["present"] = std::move(present);
                }
                if (!sh.blobs.empty()) {
                    json ents = json::object();
                    for (const auto& [g, blob] : sh.blobs) {
                        // Stored as a dumped string; embedded as a real object so the
                        // .hbsave stays readable and diffable. A blob that will not
                        // parse is dropped rather than written back as a string, which
                        // would change the shape Deserialize expects.
                        json parsed = json::parse(blob, nullptr, /*allow_exceptions*/ false);
                        if (!parsed.is_object()) continue;
                        ents[guid::ToHex(g)] = std::move(parsed);
                    }
                    if (!ents.empty()) js["entities"] = std::move(ents);
                }
                jshards[key] = std::move(js);
            }
            ja["shards"] = std::move(jshards);
        }
        jareas[name] = std::move(ja);
    }
    root["areas"] = std::move(jareas);
    return root.dump();
}

void State::Deserialize(const std::string& text) {
    areas_.clear();
    if (text.empty()) return;
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        HBE_WARN("WorldState: could not parse saved world state; starting fresh.");
        return;
    }
    const auto ait = root.find("areas");
    if (ait == root.end() || !ait->is_object()) return;

    // A v1 save has no "version" key at all.
    const u32 version = root.value("version", 1u);
    u32 droppedV1Rows = 0;

    for (const auto& [name, ja] : ait->items()) {
        if (!ja.is_object()) continue;
        AreaState a;
        a.visits = ja.value("visits", 0u);
        if (const auto v = ja.find("vars"); v != ja.end() && v->is_object())
            for (const auto& [k, jv] : v->items())
                if (jv.is_number()) a.vars[k] = jv.get<f32>();

        if (version < kFormatVersion) {
            // v1 stored per-entity rows under NAME keys. A name does not determine a
            // guid, so the rows cannot be migrated - dropping them replays the area in
            // its authored state, which is the same thing that happened before this
            // module existed. visits/vars above are keyed by area/var name and survive.
            if (const auto e = ja.find("entities"); e != ja.end() && e->is_object())
                droppedV1Rows += static_cast<u32>(e->size());
            areas_[name] = std::move(a);
            continue;
        }

        if (const auto sit = ja.find("shards"); sit != ja.end() && sit->is_object()) {
            for (const auto& [key, js] : sit->items()) {
                if (!js.is_object()) continue;
                ShardState sh;
                sh.captured = js.value("captured", false);
                if (const auto p = js.find("present"); p != js.end() && p->is_array())
                    for (const json& g : *p)
                        if (g.is_string())
                            if (const u64 v = guid::FromHex(g.get<std::string>()); v != 0)
                                sh.present.insert(v);
                if (const auto e = js.find("entities"); e != js.end() && e->is_object())
                    for (const auto& [hex, blob] : e->items()) {
                        const u64 g = guid::FromHex(hex);
                        if (g == 0 || !blob.is_object()) continue;
                        sh.blobs[g] = blob.dump();
                    }
                a.shards[key] = std::move(sh);
            }
        }
        areas_[name] = std::move(a);
    }
    if (droppedV1Rows > 0)
        HBE_WARN("WorldState: save is format v{} (pre-guid). Kept visit counts and area "
                 "variables; DROPPED {} name-keyed per-entity rows - they cannot be mapped "
                 "onto stable guids. Revisited areas replay in their authored state.",
                 version, droppedV1Rows);
}

State& Get() { return g_state; }

void SetCurrentArea(const std::string& area) { g_currentArea = area; }
const std::string& CurrentArea() { return g_currentArea; }

const std::string& ResolveArea(const std::string& area) {
    return area.empty() ? g_currentArea : area;
}

std::string AreaIdFromPath(const std::filesystem::path& p) {
    std::string s = p.stem().string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// --- The per-entity blob ------------------------------------------------------

bool Persistable(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return false;
    // Spawner-created NPCs are covered by their SPAWNER's progress, never
    // individually: a cleared camp stays cleared, individual survivors reset. This
    // is what keeps the save bounded (a continuous spawner would otherwise add a row
    // per NPC it ever emitted).
    if (reg.all_of<Spawned>(e)) return false;
    const Guid* g = reg.try_get<const Guid>(e);
    if (!g || g->value == 0) return false; // no stable key -> nothing to key state on
    return reg.any_of<Interactable, TriggerVolume, Health, Destructible, Spawner, Encounter,
                      Checkpoint, AIBehavior, AIPerception>(e);
}

std::string CaptureEntityState(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return {};
    json j = json::object();

    if (const Interactable* ia = reg.try_get<const Interactable>(e)) {
        if (ia->fired) j["interactable"] = {{"fired", true}};
    }
    if (const TriggerVolume* tv = reg.try_get<const TriggerVolume>(e)) {
        // `inside` is deliberately NOT captured: it is an enter-edge latch against a
        // player position that the respawn cannot know. Restoring true would suppress
        // the edge forever; leaving the authored false re-arms it. Seeding it from a
        // real containment test at spawn time is a distance-streaming concern.
        if (tv->fired) j["trigger"] = {{"fired", true}};
    }
    if (const Health* h = reg.try_get<const Health>(e)) {
        j["health"] = {{"current", h->current},
                       {"alive", h->alive},
                       {"deathDispatched", h->deathDispatched}};
    }
    if (const Destructible* ds = reg.try_get<const Destructible>(e)) {
        // Break progress. chunkEntity/supportScratch are deliberately absent - live
        // debris handles and a scratch buffer, both rebuilt (exactly as the scene
        // serializer's runtimeTags block documents).
        if (ds->activated) {
            j["destructible"] = {{"activated", true},
                                 {"chunkState", ds->chunkState},
                                 {"chunkHp", ds->chunkHp}};
        }
    }
    if (const AIBehavior* b = reg.try_get<const AIBehavior>(e)) {
        j["aiBehavior"] = {{"state", static_cast<u32>(b->state)},
                           {"patrolIndex", b->patrolIndex},
                           {"patrolForward", b->patrolForward},
                           {"spawnApplied", b->spawnApplied}};
    }
    if (const AIPerception* p = reg.try_get<const AIPerception>(e)) {
        // knownTarget / lastKnownPos are NOT captured: knownTarget is a raw handle
        // (meaningless after a respawn, and a dangling one is a crash) and the rest
        // is per-frame sensing that re-derives in one tick.
        j["aiPerception"] = {{"awareness", p->awareness}, {"timeSinceSeen", p->timeSinceSeen}};
    }
    if (const Spawner* sp = reg.try_get<const Spawner>(e)) {
        // SPAWNER PROGRESS ONLY. `inside` is skipped for the same enter-edge reason
        // as TriggerVolume; spawnRequested/despawnRequested are single-frame requests.
        j["spawner"] = {{"activated", sp->activated},
                        {"spawnedTotal", sp->spawnedTotal},
                        {"respawnCooldown", sp->respawnCooldown}};
    }
    if (const Encounter* en = reg.try_get<const Encounter>(e)) {
        // aliveCount is recomputed every tick from the live Spawned tags, so storing
        // it would be a lie the moment the population resets.
        // membersStreamedOut IS captured, unlike aliveCount: the encounter entity is
        // often inside the same shard as its spawners, so without it a restored
        // Active+everHadAlive encounter clears on its first tick after the respawn -
        // before its spawner has had a chance to re-burst - and awards the fight the
        // player walked out of anyway.
        j["encounter"] = {{"state", static_cast<u32>(en->state)},
                          {"everHadAlive", en->everHadAlive},
                          {"membersStreamedOut", en->membersStreamedOut}};
    }
    if (const Checkpoint* cp = reg.try_get<const Checkpoint>(e)) {
        if (cp->reached) j["checkpoint"] = {{"reached", true}};
    }

    if (j.empty()) return {};
    return j.dump();
}

void ApplyEntityState(entt::registry& reg, entt::entity e, const std::string& blob) {
    if (!reg.valid(e) || blob.empty()) return;
    const json j = json::parse(blob, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object()) {
        HBE_WARN("WorldState: malformed persisted state blob; entity left in its authored state.");
        return;
    }

    if (const auto it = j.find("interactable"); it != j.end() && it->is_object())
        if (Interactable* ia = reg.try_get<Interactable>(e)) ia->fired = it->value("fired", false);

    if (const auto it = j.find("trigger"); it != j.end() && it->is_object())
        if (TriggerVolume* tv = reg.try_get<TriggerVolume>(e)) {
            tv->fired = it->value("fired", false);
            tv->inside = false; // re-arm the enter edge; the player is not inside yet
        }

    if (const auto it = j.find("health"); it != j.end() && it->is_object())
        if (Health* h = reg.try_get<Health>(e)) {
            h->current = glm::clamp(it->value("current", h->current), 0.0f, h->max);
            h->alive = it->value("alive", true);
            // A corpse restored as dead must not re-run its one-shot death reactions
            // (flags/objectives/OnDeath) a second time on revisit. Trust the captured
            // value, but never leave a dead entity with the dispatch still armed.
            h->deathDispatched = it->value("deathDispatched", !h->alive) || !h->alive;
            if (!h->alive) {
                // Corpse presentation, which is NOT a one-shot effect: stop pathing so
                // a restored corpse does not walk (combat::Update's death branch is what
                // does this on the live edge, and it is suppressed by deathDispatched).
                if (NavigationAgent* na = reg.try_get<NavigationAgent>(e)) na->hasTarget = false;
            }
        }

    if (const auto it = j.find("destructible"); it != j.end() && it->is_object())
        if (Destructible* ds = reg.try_get<Destructible>(e)) {
            ds->activated = it->value("activated", false);
            if (const auto cs = it->find("chunkState"); cs != it->end() && cs->is_array())
                ds->chunkState = cs->get<std::vector<u8>>();
            if (const auto hp = it->find("chunkHp"); hp != it->end() && hp->is_array())
                ds->chunkHp = hp->get<std::vector<f32>>();
            // Live chunk handles do not survive. Sizing them here keeps chunkEntity[]
            // index-parallel; destruction::Update is what actually REBUILDS them, and
            // `reactivate` is the only thing that lets it - Activate() early-returns on
            // an already-activated object, and Activate() is also what strips the
            // root's intact MeshInstance/RigidBody, so without this the restored wall
            // renders and collides as pristine while being internally fully broken.
            ds->chunkEntity.assign(ds->chunkState.size(), entt::entity{entt::null});
            ds->supportScratch.clear();
            // NOT structureDirty: the captured chunkState is already the SOLVED state
            // (it was captured after ResolveSupport ran). Re-solving against an empty
            // chunkEntity[] would cascade the object to Detached with nothing visible
            // happening. The rebuild below re-establishes it exactly.
            ds->structureDirty = false;
            ds->reactivate = ds->activated;
        }

    if (const auto it = j.find("aiBehavior"); it != j.end() && it->is_object())
        if (AIBehavior* b = reg.try_get<AIBehavior>(e)) {
            b->state = static_cast<AIState>(
                std::min(it->value("state", 0u), static_cast<u32>(AIState::Dead)));
            b->patrolIndex = it->value("patrolIndex", 0u);
            b->patrolForward = it->value("patrolForward", true);
            b->spawnApplied = it->value("spawnApplied", false);
            b->prevState = b->state; // no spurious OnSpotPlayer edge on the restore frame
        }

    if (const auto it = j.find("aiPerception"); it != j.end() && it->is_object())
        if (AIPerception* p = reg.try_get<AIPerception>(e)) {
            p->awareness = glm::clamp(it->value("awareness", 0.0f), 0.0f, 1.0f);
            p->timeSinceSeen = it->value("timeSinceSeen", 999.0f);
            p->knownTarget = entt::null; // a handle from the previous life means nothing
            p->canSeeTarget = false;
            p->hasLastKnownPos = false;
            p->heardSomething = false;
        }

    if (const auto it = j.find("spawner"); it != j.end() && it->is_object())
        if (Spawner* sp = reg.try_get<Spawner>(e)) {
            sp->activated = it->value("activated", false);
            sp->spawnedTotal = it->value("spawnedTotal", 0u);
            sp->respawnCooldown = it->value("respawnCooldown", 0.0f);
            sp->inside = false; // re-arm the enter edge (see CaptureEntityState)
            sp->spawnRequested = false;
            sp->despawnRequested = false;
        }

    if (const auto it = j.find("encounter"); it != j.end() && it->is_object())
        if (Encounter* en = reg.try_get<Encounter>(e)) {
            en->state = static_cast<Encounter::State>(
                std::min(it->value("state", 0u), static_cast<u32>(Encounter::State::Cleared)));
            en->everHadAlive = it->value("everHadAlive", false);
            en->aliveCount = 0;
            en->clearedEdge = false;
            en->activateRequested = false;
            // The population was unloaded, not killed. Keeping this across the respawn
            // is what stops an ABANDONED (not cleared) fight firing its completion
            // action on the first tick back, before its re-armed spawner has burst.
            // spawn::UpdateEncounters clears it the moment live members exist again.
            en->membersStreamedOut = it->value("membersStreamedOut", false);
            // A CLEARED encounter must not re-fire its cleared action. Its population is
            // deliberately not persisted, so on respawn aliveCount is 0 - and the
            // Cleared state above is terminal in spawn::UpdateEncounters, which is what
            // stops the completion cutscene/objective firing a second time.
        }

    if (const auto it = j.find("checkpoint"); it != j.end() && it->is_object())
        if (Checkpoint* cp = reg.try_get<Checkpoint>(e)) cp->reached = it->value("reached", false);
}

// --- Area / shard lifecycle ---------------------------------------------------

void EnterArea(const std::string& area) {
    if (area.empty()) return;
    SetCurrentArea(area); // script nodes can now pass "" to mean "here"
    const u32 visit = ++g_state.Area(area).visits;
    // Logged because "the current area is empty" was the whole of blocker B5: until the
    // shipping load path called this, CurrentArea() was permanently "" and the World
    // schematic nodes were silently dead. One line per level entry makes it observable.
    HBE_INFO("WorldState: entered area '{}' (visit {}).", area, visit);
}

void CaptureGroup(const Scene& scene, const std::string& area, const std::string& shardKey,
                  const std::vector<entt::entity>& members) {
    if (area.empty() || shardKey.empty()) return;
    const entt::registry& reg = scene.Registry();

    std::unordered_set<u64> present;
    std::unordered_map<u64, std::string> blobs;
    u32 skippedNoGuid = 0;
    for (const entt::entity e : members) {
        if (!reg.valid(e)) continue;
        if (!Persistable(reg, e)) {
            // An entity that carries changeable state but no guid can never be
            // restored. Worth counting: it means a scene file was never guid-stamped.
            if (reg.valid(e) && !reg.all_of<Spawned>(e) && GuidOf(reg, e) == 0 &&
                reg.any_of<Interactable, TriggerVolume, Health, Destructible, Spawner, Encounter,
                           Checkpoint, AIBehavior, AIPerception>(e))
                ++skippedNoGuid;
            continue;
        }
        const u64 g = GuidOf(reg, e);
        present.insert(g);
        if (std::string blob = CaptureEntityState(reg, e); !blob.empty())
            blobs[g] = std::move(blob);
    }

    AreaState& a = g_state.Area(area);
    ShardState& sh = a.shards[shardKey];
    // A capture that finds NOTHING must not overwrite a real one. Capturing a shard
    // whose members are already destroyed (a double despawn, a teardown after the
    // world was cleared) would otherwise replace "everything here is dead" with "this
    // shard was never touched" - a silent strip of the player's progress.
    if (present.empty() && sh.captured && !sh.present.empty()) {
        HBE_WARN("WorldState: refused to capture an EMPTY member set over area '{}' shard '{}' "
                 "({} guids already recorded). Existing state kept.",
                 area, shardKey, sh.present.size());
        return;
    }
    sh.captured = true;
    sh.present = std::move(present);
    sh.blobs = std::move(blobs);
    if (skippedNoGuid > 0)
        HBE_WARN("WorldState: area '{}' shard '{}': {} entities carry changeable state but no "
                 "guid, so their state cannot persist. Run --migrate-guids on the project.",
                 area, shardKey, skippedNoGuid);
    HBE_TRACE("WorldState: captured area '{}' shard '{}' ({} tracked, {} with deltas).", area,
             shardKey, sh.present.size(), sh.blobs.size());
}

void RestoreGroup(Scene& scene, const std::string& area, const std::string& shardKey,
                  const std::vector<entt::entity>& members) {
    if (area.empty() || shardKey.empty()) return;
    const ShardState* saved = g_state.FindShard(area, shardKey);
    if (!saved || !saved->captured) return; // first visit: nothing to replay
    // Copy the two lookups out: DestroySubtree below can invalidate nothing in
    // g_state, but the shard row is reached through a map that a nested restore
    // could rehash. Cheap, and it makes the loop obviously safe.
    const std::unordered_set<u64> present = saved->present;
    const std::unordered_map<u64, std::string> blobs = saved->blobs;

    entt::registry& reg = scene.Registry();

    // Snapshot (guid, entity) first: the destroy pass below mutates the registry, so
    // walking `members` while destroying out of it would be walking a live edit.
    std::vector<std::pair<u64, entt::entity>> loaded;
    loaded.reserve(members.size());
    for (const entt::entity e : members) {
        if (!reg.valid(e) || !Persistable(reg, e)) continue;
        loaded.emplace_back(GuidOf(reg, e), e);
    }

    usize destroyed = 0, restored = 0;
    for (const auto& [g, e] : loaded) {
        if (!reg.valid(e)) continue; // already taken by an earlier subtree destroy
        if (!present.count(g)) {
            // Present in the authored file, absent at capture time => the player
            // destroyed it during the visit (looted, consumed). Destroy it again so
            // the world stays as they left it.
            //
            // The PLAYER is never re-destroyed: a run where the player entity was
            // destroyed must not delete the freshly loaded one and leave an unplayable
            // world. Nothing destroys the player today (death sets alive=false), so
            // this is a guard, not a workaround.
            if (reg.all_of<CharacterController>(e)) {
                HBE_WARN("WorldState: area '{}' shard '{}': not re-destroying the player entity.",
                         area, shardKey);
                continue;
            }
            DestroySubtree(reg, e); // takes mesh/collider children with it
            ++destroyed;
            continue;
        }
        const auto it = blobs.find(g);
        if (it == blobs.end()) continue; // tracked, but carried no delta
        ApplyEntityState(reg, e, it->second);
        ++restored;
    }
    HBE_TRACE("WorldState: restored area '{}' shard '{}' ({} re-destroyed, {} re-applied).", area,
             shardKey, destroyed, restored);
}

// --- Whole-scene convenience --------------------------------------------------

namespace {
// The always-resident set: every entity that is NOT a member of a streamed shard.
// Scoping the whole-scene capture/restore this way is what stops a level with a
// despawned shard from recording that shard's absent entities as destroyed.
std::vector<entt::entity> ResidentMembers(const entt::registry& reg) {
    std::vector<entt::entity> out;
    for (const entt::entity e : reg.view<const Guid>()) {
        if (reg.all_of<StreamShard>(e)) continue;
        if (!Persistable(reg, e)) continue;
        out.push_back(e);
    }
    return out;
}
} // namespace

void CaptureArea(const Scene& scene, const std::string& area) {
    if (area.empty()) return;
    CaptureGroup(scene, area, kResidentKey, ResidentMembers(scene.Registry()));
}

void RestoreArea(Scene& scene, const std::string& area) {
    if (area.empty()) return;
    EnterArea(area);
    RestoreGroup(scene, area, kResidentKey, ResidentMembers(scene.Registry()));
}

} // namespace hbe::world
