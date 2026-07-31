// Scene/TagStreaming.cpp
#include "Scene/TagStreaming.h"

#include "Scene/StreamingSalvage.h" // SALVAGE 2 (hysteresis), 3 (finalize budget), 4 (settle)

#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/EntityGuid.h"
#include "Scene/Scene.h"
#include "Scene/TagTable.h"
#include "Scene/WorldState.h"

#include "Schematic/SchematicSystem.h" // BakedEnt/BakedGet - the raw-bits guard invariant 3 names

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_set>

namespace hbe::stream {

namespace {

// An empty slice must still be a SLICE. scene::Instantiate/StageAssets treat a NULL
// index pointer as "the whole file", so a zero-length std::vector's data() (which may
// be null) would silently load the entire level. Every call goes through this.
constexpr u32 kNoRows = 0;
const u32* SlicePtr(const std::vector<u32>& rows) { return rows.empty() ? &kNoRows : rows.data(); }

// The project's config for a tag NAME, or TagDef's defaults when the project does
// not list it. A tag the scene references but the project does not list is STREAMED
// with default radii, matching TagDef: tags::ReconcileWithTable is what surfaces it to
// the author, and silently promoting it to always-loaded here would hide the
// authoring mistake behind "it works".
TagDef DefForTag(const std::vector<TagDef>& defs, const std::string& name) {
    for (const TagDef& d : defs)
        if (d.name == name) return d;
    TagDef fallback;
    fallback.name = name;
    return fallback;
}

bool IsAlwaysLoadedTag(const std::vector<TagDef>& defs, const std::string& name) {
    return DefForTag(defs, name).alwaysLoaded;
}

// Scrubs every raw entt::entity field in SURVIVING components that points into
// `dying`. EnTT bumps the version on destroy, so a stale handle reads as invalid
// rather than as the wrong object - but `try_get` on an invalid handle is an
// ENTT_ASSERT in Debug and an out-of-bounds sparse-set index in Release, and several
// of these fields are read WITHOUT a validity test (Health::lastAttacker straight into
// game::DeathRec::instigator, AIPerception::knownTarget straight into
// game::SpottedRec::target). Nulling them at the source is the fix that does not
// depend on every future reader remembering.
void ScrubDanglingHandles(Scene& scene, const std::unordered_set<u32>& dying) {
    entt::registry& reg = scene.Registry();
    const auto isDying = [&dying](entt::entity e) {
        return e != entt::null && dying.count(static_cast<u32>(e)) > 0;
    };
    const auto survives = [&dying](entt::entity e) {
        return dying.count(static_cast<u32>(e)) == 0;
    };

    for (const entt::entity e : reg.view<Health>()) {
        if (!survives(e)) continue;
        Health& h = reg.get<Health>(e);
        if (isDying(h.lastAttacker)) h.lastAttacker = entt::null;
    }
    for (const entt::entity e : reg.view<AIPerception>()) {
        if (!survives(e)) continue;
        AIPerception& p = reg.get<AIPerception>(e);
        if (isDying(p.knownTarget)) {
            p.knownTarget = entt::null;
            p.canSeeTarget = false; // the sensing that produced it is gone with the target
        }
    }
    for (const entt::entity e : reg.view<Destructible>()) {
        if (!survives(e)) continue;
        Destructible& ds = reg.get<Destructible>(e);
        for (entt::entity& ce : ds.chunkEntity)
            if (isDying(ce)) ce = entt::null;
    }
    for (const entt::entity e : reg.view<DebrisChunk>()) {
        if (!survives(e)) continue;
        DebrisChunk& dc = reg.get<DebrisChunk>(e);
        if (isDying(dc.owner)) dc.owner = entt::null;
    }
    for (const entt::entity e : reg.view<Character>()) {
        if (!survives(e)) continue;
        Character& ch = reg.get<Character>(e);
        for (auto it = ch.liveParts.begin(); it != ch.liveParts.end();)
            it = isDying(it->second) ? ch.liveParts.erase(it) : std::next(it);
    }
    for (const entt::entity e : reg.view<SkinnedPartRef>()) {
        if (!survives(e)) continue;
        SkinnedPartRef& pr = reg.get<SkinnedPartRef>(e);
        if (isDying(pr.character)) pr.character = entt::null;
    }
    for (const entt::entity e : reg.view<UICanvas>()) {
        if (!survives(e)) continue;
        UICanvas& c = reg.get<UICanvas>(e);
        if (isDying(c.surface)) c.surface = entt::null;
    }
    for (const entt::entity e : reg.view<UISurface>()) {
        if (!survives(e)) continue;
        UISurface& s = reg.get<UISurface>(e);
        if (isDying(s.canvas)) s.canvas = entt::null;
    }
    // A surviving child whose PARENT dies. The closure below is transitive over Parent
    // links, so this should be unreachable; it is kept as a guard because the failure
    // it prevents is not a crash but a silent teleport - a de-parented child renders at
    // its LOCAL transform in world space, which reads as a content bug. Same rule the
    // slice loader applies to a cross-slice parent: become a root, loudly.
    std::vector<entt::entity> orphaned;
    for (const entt::entity e : reg.view<Parent>()) {
        if (!survives(e)) continue;
        if (isDying(reg.get<Parent>(e).entity)) orphaned.push_back(e);
    }
    for (const entt::entity e : orphaned) {
        const Name* n = reg.try_get<Name>(e);
        HBE_WARN("TagStream: '{}' survives a despawn that took its parent; it becomes a ROOT at "
                 "its local transform. The shard closure should have included it.",
                 n ? n->value : std::string("<unnamed>"));
        reg.remove<Parent>(e);
    }
}

// STREAMING MUST BE INVISIBLE TO ENCOUNTER LOGIC. spawn::UpdateEncounters recomputes
// aliveCount every tick from live `Spawned` tags and treats "was populated, now zero"
// as CLEARED - and gameplay::Update runs in the same frame as UpdateTagStreaming. So a
// despawn that takes a half-fought camp's survivors would, on the very next tick, fire
// the encounter's cleared cutscene / objective / flag and award the player a fight they
// walked out of. Two edges, both of them here because only the despawn knows WHY the
// members disappeared:
//
//   * every encounter whose live members are in this closure is marked
//     membersStreamedOut, which suppresses the clear transition; and
//   * every spawner whose still-LIVE population is in this closure is RE-ARMED
//     (activated/inside/cooldown reset) so re-entering the area repopulates it. That is
//     user decision 4 read literally: spawner PROGRESS persists, individual survivors'
//     health resets. A spawner whose members were all already dead is NOT re-armed -
//     the player wiped that camp, and resurrecting it is the opposite failure.
//
// Runs BEFORE world::CaptureGroup, so the captured spawner progress records the
// re-armed state rather than the state that was just invalidated.
void MarkStreamedOutPopulation(Scene& scene, const std::vector<entt::entity>& members) {
    entt::registry& reg = scene.Registry();
    std::unordered_set<std::string> encounters, liveSpawners;
    for (const entt::entity e : members) {
        if (!reg.valid(e)) continue;
        const Spawned* sp = reg.try_get<const Spawned>(e);
        if (!sp) continue;
        const Health* h = reg.try_get<const Health>(e);
        const bool alive = !h || h->alive; // spawn::IsAliveMember's rule
        if (!alive) continue;
        if (!sp->encounterId.empty()) encounters.insert(sp->encounterId);
        if (!sp->spawnerId.empty()) liveSpawners.insert(sp->spawnerId);
    }
    if (encounters.empty() && liveSpawners.empty()) return;

    for (const entt::entity e : reg.view<Encounter>()) {
        Encounter& en = reg.get<Encounter>(e);
        if (en.state == Encounter::State::Cleared) continue; // already resolved; leave it
        if (encounters.count(en.id)) en.membersStreamedOut = true;
    }
    for (const entt::entity e : reg.view<Spawner>()) {
        Spawner& s = reg.get<Spawner>(e);
        if (!liveSpawners.count(s.spawnerId)) continue;
        // A cleared encounter's spawner stays spent: the fight is over.
        bool cleared = false;
        if (!s.encounterId.empty()) {
            for (const entt::entity ee : reg.view<const Encounter>()) {
                const Encounter& en = reg.get<const Encounter>(ee);
                if (en.id == s.encounterId && en.state == Encounter::State::Cleared)
                    cleared = true;
            }
        }
        if (cleared) continue;
        s.activated = false;
        s.inside = false; // re-arm the Volume enter-edge; the player is not inside yet
        s.respawnCooldown = 0.0f;
        s.spawnRequested = false;
        s.despawnRequested = false;
    }
}

// Destroys a collected closure. Physics bodies, character controllers and spatial
// audio voices are reaped LAZILY by their own systems on the next Update (they scan
// their side-tables for handles that are invalid or whose id no longer matches), nav
// obstacles and particle passes are rebuilt from live views every frame, so nothing
// here has to notify them. GPU meshes and textures are NOT reclaimed - the RHI has no
// destroy for either.
usize DestroyClosure(Scene& scene, const std::vector<entt::entity>& members) {
    entt::registry& reg = scene.Registry();
    std::unordered_set<u32> dying;
    dying.reserve(members.size() * 2);
    for (const entt::entity e : members)
        if (reg.valid(e)) dying.insert(static_cast<u32>(e));

    ScrubDanglingHandles(scene, dying);

    usize destroyed = 0;
    for (const entt::entity e : members) {
        if (!reg.valid(e)) continue;
        // Before the destroy: EnTT recycles ids, so a respawn landing on this slot
        // would inherit the dead object's previous world matrix and emit one frame of
        // bogus motion vectors (a TAA smear between the two positions).
        scene.ForgetMotionHistory(e);
        reg.destroy(e);
        ++destroyed;
    }
    return destroyed;
}

} // namespace

const char* ToString(Residency r) {
    switch (r) {
        case Residency::Resident:     return "Resident";
        case Residency::StreamedOut:  return "StreamedOut";
        case Residency::AlwaysLoaded: return "AlwaysLoaded";
        case Residency::NotInLevel:   break;
    }
    return "NotInLevel";
}

void CollectShardEntities(const Scene& scene, u32 shard, std::vector<entt::entity>& out) {
    out.clear();
    const entt::registry& reg = scene.Registry();

    std::unordered_set<u32> set;
    for (const entt::entity e : reg.view<const StreamShard>())
        if (reg.get<const StreamShard>(e).index == shard) set.insert(static_cast<u32>(e));
    if (set.empty()) return;

    const auto has = [&set](entt::entity e) {
        return e != entt::null && set.count(static_cast<u32>(e)) > 0;
    };

    // Fixed point: each rule can pull in an entity another rule then follows. The
    // shape is spawn::DespawnBySpawner's loop, widened from Parent alone to the five
    // other links that tie a created-later entity back to its origin.
    bool grew = true;
    while (grew) {
        grew = false;
        const auto add = [&](entt::entity e) {
            if (set.insert(static_cast<u32>(e)).second) grew = true;
        };

        for (const entt::entity e : reg.view<const Parent>())
            if (!has(e) && has(reg.get<const Parent>(e).entity)) add(e);

        // Spawned NPCs, tied back by STRING spawner id (they are created long after the
        // shard loaded, and their StreamShard is inherited only if spawn:: copies it).
        std::unordered_set<std::string> spawnerIds;
        for (const entt::entity e : reg.view<const Spawner>()) {
            if (!has(e)) continue;
            const std::string& id = reg.get<const Spawner>(e).spawnerId;
            if (!id.empty()) spawnerIds.insert(id);
        }
        if (!spawnerIds.empty())
            for (const entt::entity e : reg.view<const Spawned>())
                if (!has(e) && spawnerIds.count(reg.get<const Spawned>(e).spawnerId)) add(e);

        // Destruction debris: DETACHED chunks have their Parent removed, so the Parent
        // closure cannot find them and a despawn would strand them in the world forever.
        for (const entt::entity e : reg.view<const DebrisChunk>())
            if (!has(e) && has(reg.get<const DebrisChunk>(e).owner)) add(e);
        for (const entt::entity e : reg.view<const Destructible>()) {
            if (!has(e)) continue;
            for (const entt::entity ce : reg.get<const Destructible>(e).chunkEntity)
                if (ce != entt::null && reg.valid(ce) && !has(ce)) add(ce);
        }

        // World-space UI: the render-target quad and its canvas, either direction.
        for (const entt::entity e : reg.view<const UISurface>())
            if (!has(e) && has(reg.get<const UISurface>(e).canvas)) add(e);
        for (const entt::entity e : reg.view<const UICanvas>()) {
            if (!has(e)) continue;
            const entt::entity s = reg.get<const UICanvas>(e).surface;
            if (s != entt::null && reg.valid(s) && !has(s)) add(s);
        }

        // Modular-character parts: created by character::Instantiate AFTER Instantiate
        // filled its createdOut, which is precisely why a load-list replay leaks them.
        for (const entt::entity e : reg.view<const SkinnedPartRef>())
            if (!has(e) && has(reg.get<const SkinnedPartRef>(e).character)) add(e);
        for (const entt::entity e : reg.view<const Character>()) {
            if (!has(e)) continue;
            for (const auto& [slot, part] : reg.get<const Character>(e).liveParts)
                if (part != entt::null && reg.valid(part) && !has(part)) add(part);
        }
    }

    out.reserve(set.size());
    for (const u32 bits : set) {
        const entt::entity e = static_cast<entt::entity>(bits);
        if (reg.valid(e)) out.push_back(e);
    }
    // Deterministic order: the destroy order is otherwise hash-order, which makes a
    // log or a test result depend on allocator addresses.
    std::sort(out.begin(), out.end(),
              [](entt::entity a, entt::entity b) { return static_cast<u32>(a) < static_cast<u32>(b); });
}

// --- Focus points -------------------------------------------------------------

void FocusPoints(const Scene& scene, const glm::vec3& cameraPos, std::vector<glm::vec3>& out) {
    out.clear();
    const entt::registry& reg = scene.Registry();
    // The player, if this world has one. Same rule spawn::PlayerPos uses: the first
    // CharacterController. A menu or a cutscene-only scene simply has none.
    // (Written as a first-element test rather than a loop with an unconditional break:
    // MSVC flags the loop's unreachable increment as C4702 in Debug.)
    const auto players = reg.view<const CharacterController>();
    if (players.begin() != players.end())
        out.push_back(glm::vec3(scene.WorldMatrix(*players.begin())[3]));
    // The active camera, ALWAYS - and not as a fallback. A cutscene camera flies
    // ahead of a stationary player; a camera zone can frame a place the player never
    // reaches. Streaming to the player alone pops geometry in on camera.
    out.push_back(cameraPos);
}

// --- Streamer -----------------------------------------------------------------

Streamer::~Streamer() {
    // A staging job holds a raw ShardRuntime* into shards_. Letting the vector free
    // while one is running is exactly the use-after-free the deleted cell streamer
    // had (plan blocker B13).
    DrainInFlight();
}

void Streamer::DrainInFlight() const {
    // Jobs are fire-and-forget (jobs::RunDetached has no counter to wait on), so the
    // in-flight count IS the join. A staging job only reads source_/assetsDir_ and
    // writes its own shard, so a spin here is bounded by one StageAssets call.
    while (inFlight_.load(std::memory_order_acquire) != 0) std::this_thread::yield();
}

void Streamer::PublishResidency(Scene& scene) const {
    StreamingResidency r;
    r.bound = bound_;
    r.shardCount = static_cast<u32>(shards_.size());
    if (bound_) {
        for (usize i = 0; i < shards_.size(); ++i) {
            if (shards_[i]->resident) continue;
            ++r.nonResident;
            // Name the first few. The message this feeds is an author-facing refusal,
            // so it has to say WHAT is missing; the whole list of a big level would
            // bury that under itself.
            if (r.nonResident <= 6) {
                if (!r.missing.empty()) r.missing += ", ";
                r.missing += ShardKey(static_cast<u32>(i));
            } else if (r.nonResident == 7) {
                r.missing += ", ...";
            }
        }
    }
    scene.SetStreaming(std::move(r));
}

void Streamer::Reset(Scene* scene) {
    DrainInFlight();
    bound_ = false;
    trusted_ = true;
    untrustedReason_.clear();
    levelPath_.clear();
    assetsDir_.clear();
    areaId_.clear();
    source_ = scene::SceneData{};
    shards_.clear();
    residentRows_.clear();
    guidToShard_.clear();
    nameToRow_.clear();
    rowToShard_.clear();
    frame_ = 0;
    lastEvalFrame_ = 0;
    lastEvalFoci_.clear();
    forceEval_ = true;
    policyScratch_.clear();
    if (scene) PublishResidency(*scene); // bound_ is false now: clears the summary
}

std::string Streamer::ShardKey(u32 i) const {
    if (i >= shards_.size()) return {};
    return shards_[i]->desc.tag + "#" + std::to_string(shards_[i]->desc.index);
}

u32 Streamer::ResidentShardCount() const {
    u32 n = 0;
    for (const auto& sr : shards_)
        if (sr->resident) ++n;
    return n;
}

std::vector<std::string> Streamer::ResidentKeys() const {
    std::vector<std::string> keys;
    for (usize i = 0; i < shards_.size(); ++i)
        if (shards_[i]->resident) keys.push_back(ShardKey(static_cast<u32>(i)));
    // Sorted so a save file's shard list is stable and diffable rather than depending
    // on the bake's shard order changing under it.
    std::sort(keys.begin(), keys.end());
    return keys;
}

i32 Streamer::FindShard(const std::string& key) const {
    for (usize i = 0; i < shards_.size(); ++i)
        if (ShardKey(static_cast<u32>(i)) == key) return static_cast<i32>(i);
    return -1;
}

bool Streamer::BindLevel(Scene& scene, Renderer& renderer, const std::filesystem::path& sceneFile,
                         const std::filesystem::path& assetsDir, const std::vector<TagDef>& defs,
                         BindMode mode) {
    if (sceneFile.empty()) return false;
    // Parse into a LOCAL first. A failed parse must leave the existing binding - and the
    // world it describes - completely untouched: half-binding would destroy the current
    // level and then have nothing to put in its place.
    scene::SceneData parsed;
    if (!scene::ParseSceneFile(sceneFile, parsed)) {
        HBE_ERROR("TagStream: cannot bind '{}' (parse failed); the current binding is kept.",
                  sceneFile.string());
        return false;
    }
    // Leaving a level: capture what the player changed in it before BindWorld destroys
    // it. Without this, a level transition silently discards every delta the outgoing
    // level's resident set and shards were holding. Skipped for AdoptWorld: the world
    // in the registry there is the INCOMING one (a save snapshot), not the outgoing
    // one, so "capturing" it would record the destination over the origin.
    if (bound_ && mode == BindMode::Fresh) UnloadAll(scene);
    Reset(&scene);
    source_ = std::move(parsed);
    levelPath_ = sceneFile;
    assetsDir_ = assetsDir;
    areaId_ = world::AreaIdFromPath(sceneFile);

    const tagshard::ParsedShards ps = tagshard::FromParsed(source_);
    trusted_ = ps.trusted;
    untrustedReason_ = ps.reason;

    rowToShard_.assign(source_.entities.size(), -1);
    if (trusted_) {
        for (usize s = 0; s < ps.shards.size(); ++s) {
            // An alwaysLoaded tag's shards are real shards in the file (the bake still
            // computes their bounds, which is useful as a diagnostic) but they are never
            // streamed: their rows join the resident slice.
            const TagDef def = DefForTag(defs, ps.shards[s].tag);
            if (def.alwaysLoaded) continue;
            auto sr = std::make_unique<ShardRuntime>();
            sr->desc = ps.shards[s];
            sr->rows = ps.members[s];
            // Resolve the band ONCE, here, so the per-frame policy never looks a tag
            // up. tags::Normalize already corrected it at parse; re-applying the clamp
            // costs nothing and means a hand-built TagDef (a test, a project file
            // written by an older editor) cannot hand the policy a thrashing band.
            sr->loadRadius = glm::max(def.loadRadius, 0.0f);
            sr->unloadRadius = def.unloadRadius;
            salvage::EnforceHysteresis(sr->loadRadius, sr->unloadRadius);
            sr->priority = def.priority;
            sr->owner = this;
            const i32 idx = static_cast<i32>(shards_.size());
            for (const u32 r : sr->rows)
                if (r < rowToShard_.size()) rowToShard_[r] = idx;
            shards_.push_back(std::move(sr));
        }
    }
    for (usize r = 0; r < source_.entities.size(); ++r)
        if (rowToShard_[r] < 0) residentRows_.push_back(static_cast<u32>(r));

    for (usize r = 0; r < source_.entities.size(); ++r) {
        const scene::EntityData& d = source_.entities[r];
        // First row wins for a duplicate, matching Scene::FindByName's documented
        // arbitrary-match behaviour rather than inventing a second rule.
        if (d.guid != 0) guidToShard_.emplace(d.guid, rowToShard_[r]);
        if (!d.name.empty()) nameToRow_.emplace(d.name, static_cast<u32>(r));
    }

    bound_ = true;
    forceEval_ = true; // the first Update after a bind evaluates, cadence or not

    if (mode == BindMode::Fresh) {
        // THE LEVEL owns the environment, not a shard: this clears the previous world
        // and applies ambient/exposure/shadowDistance/post/giSource exactly as a full
        // LoadMode::Replace would, which a slice may never do. Calling BindLevel twice
        // re-binds rather than stacking a second world.
        scene::BindWorld(scene, renderer, source_);
        world::EnterArea(areaId_); // one visit per area entry, before any RestoreGroup
        SpawnRows(scene, renderer, residentRows_, world::kResidentKey, -1, nullptr);
    } else {
        // AdoptWorld: the registry already holds the world (a `.hbsave` snapshot). Make
        // the area current WITHOUT bumping its visit count - the player did not
        // re-enter it, they reloaded inside it - and instantiate nothing. AdoptResidency
        // is what reconciles residency next.
        world::SetCurrentArea(areaId_);
    }

    HBE_INFO("TagStream: bound '{}' as area '{}' - {} resident rows, {} streamed shard(s){}{}.",
             sceneFile.filename().string(), areaId_, residentRows_.size(), shards_.size(),
             trusted_ ? "" : " (shard table UNTRUSTED: everything resident)",
             mode == BindMode::AdoptWorld ? " (adopting a restored world)" : "");
    // The world in this Scene is now a STREAMED world: complete only while every
    // shard happens to be spawned. Anything that writes it back to an authored file
    // reads this (see Scene::Streaming / scene::SaveRefusal).
    PublishResidency(scene);
    return true;
}

u32 Streamer::AdoptResidency(Scene& scene, Renderer& renderer,
                             const std::vector<std::string>& residentKeys) {
    if (!bound_) return 0;
    entt::registry& reg = scene.Registry();

    // 1) Adopt: stamp membership onto whatever the restored world already contains.
    std::vector<u32> found(shards_.size(), 0u);
    for (const entt::entity e : reg.view<const Guid>()) {
        const auto it = guidToShard_.find(reg.get<const Guid>(e).value);
        if (it == guidToShard_.end() || it->second < 0) continue; // resident or not ours
        const u32 idx = static_cast<u32>(it->second);
        reg.emplace_or_replace<StreamShard>(e, StreamShard{idx});
        ++found[idx];
    }
    u32 adopted = 0;
    for (usize i = 0; i < shards_.size(); ++i) {
        if (found[i] == 0) continue;
        shards_[i]->resident = true;
        shards_[i]->state.store(static_cast<int>(salvage::RegionState::Loaded),
                                std::memory_order_release);
        ++adopted;
        // A PARTIAL shard is still resident - the entities exist and the streamer has
        // to be able to despawn them - but it is worth saying, because the usual cause
        // is a snapshot written against a different bake of the same level.
        if (found[i] != shards_[i]->desc.count)
            HBE_WARN("TagStream: adopted shard '{}' with {} of {} members; the save was "
                     "written against a different bake of this level.",
                     ShardKey(static_cast<u32>(i)), found[i], shards_[i]->desc.count);
    }

    // 2) Spawn the recorded-resident shards the snapshot did NOT carry. Skipping an
    // already-adopted key is the double-spawn guard.
    u32 spawned = 0;
    for (const std::string& key : residentKeys) {
        const i32 idx = FindShard(key);
        if (idx < 0) {
            HBE_WARN("TagStream: save names resident shard '{}', which this level does not "
                     "contain; ignoring it.",
                     key);
            continue;
        }
        if (shards_[static_cast<usize>(idx)]->resident) continue; // adopted above
        if (SpawnShard(scene, renderer, static_cast<u32>(idx))) ++spawned;
    }
    HBE_INFO("TagStream: residency restored - {} shard(s) adopted from the snapshot, {} "
             "re-spawned from the level.",
             adopted, spawned);
    PublishResidency(scene);
    return spawned;
}

void Streamer::SpawnRows(Scene& scene, Renderer& renderer, const std::vector<u32>& rows,
                         const std::string& shardKey, i32 shardIndex,
                         std::vector<entt::entity>* createdOut) {
    scene::StagedAssets staged;
    scene::StageAssets(source_, assetsDir_, staged, SlicePtr(rows), static_cast<u32>(rows.size()));

    std::vector<entt::entity> created;
    scene::Instantiate(scene, renderer, source_, staged, scene::LoadMode::Additive, &created,
                       /*sceneTag*/ {}, SlicePtr(rows), static_cast<u32>(rows.size()));

    if (shardIndex >= 0) {
        entt::registry& reg = scene.Registry();
        for (const entt::entity e : created)
            if (reg.valid(e))
                reg.emplace_or_replace<StreamShard>(e, StreamShard{static_cast<u32>(shardIndex)});
    }
    // Replay the captured deltas the same frame the entities appear, so nothing ever
    // observes the authored state of a shard the player already changed.
    world::RestoreGroup(scene, areaId_, shardKey, created);
    if (createdOut) *createdOut = std::move(created);
}

bool Streamer::SpawnShard(Scene& scene, Renderer& renderer, u32 i) {
    if (!bound_ || i >= shards_.size()) return false;
    ShardRuntime& sr = *shards_[i];
    if (sr.resident) return false;
    // The SYNCHRONOUS path: stage and instantiate in one call. Used by a manual spawn
    // (the editor, a save restore, a self-test) and as the fallback when the job system
    // is not running. Automatic streaming goes through StageJob + Finalize instead so
    // the parse/IO half is off the main thread.
    ++stats_.syncStages;
    SpawnRows(scene, renderer, sr.rows, ShardKey(i), static_cast<i32>(i), &sr.lastCreated);
    sr.resident = true;
    sr.state.store(static_cast<int>(salvage::RegionState::Loaded), std::memory_order_release);
    ++stats_.spawns;
    stats_.residentPeak = glm::max(stats_.residentPeak, ResidentShardCount());
    PublishResidency(scene);
    HBE_TRACE("TagStream: spawned shard '{}' ({} rows, {} entities).", ShardKey(i),
             sr.rows.size(), sr.lastCreated.size());
    return true;
}

bool Streamer::DespawnShard(Scene& scene, u32 i) {
    if (!bound_ || i >= shards_.size()) return false;
    ShardRuntime& sr = *shards_[i];
    if (!sr.resident) return false;

    std::vector<entt::entity> members;
    CollectShardEntities(scene, i, members);
    // Encounters/spawners first: this is what stops a walked-away-from fight reading as
    // CLEARED on the next gameplay tick, and it has to run before the capture so the
    // re-armed spawner progress is what gets recorded.
    MarkStreamedOutPopulation(scene, members);
    // CAPTURE FIRST, ALWAYS. Everything below this line is irreversible, and a despawn
    // that destroys before it records is exactly the regression that made automatic
    // streaming unshippable without this step.
    world::CaptureGroup(scene, areaId_, ShardKey(i), members);
    const usize destroyed = DestroyClosure(scene, members);

    sr.resident = false;
    sr.lastCreated.clear();
    sr.state.store(static_cast<int>(salvage::RegionState::Unloaded), std::memory_order_release);
    ++stats_.despawns;
    // Entities, draw items, physics bodies, audio voices and CPU memory - and ZERO
    // VRAM (the RHI has no mesh/texture destroy). Said here so the log cannot imply
    // otherwise.
    PublishResidency(scene); // the world is now INCOMPLETE; the save path must know
    HBE_TRACE("TagStream: despawned shard '{}' ({} entities; no GPU memory is reclaimed).",
             ShardKey(i), destroyed);
    return true;
}

void Streamer::UnloadAll(Scene& scene) {
    if (!bound_) return;
    // A shard mid-flight must not be left with a job pointing at it while the level
    // goes away, and a shard that finished staging must not be finalized into a world
    // that is being torn down.
    DrainInFlight();
    for (auto& sr : shards_) {
        const auto s = static_cast<salvage::RegionState>(sr->state.load(std::memory_order_acquire));
        if (s == salvage::RegionState::Ready || s == salvage::RegionState::Loading) {
            sr->staged = scene::StagedAssets{};
            sr->state.store(static_cast<int>(salvage::RegionState::Unloaded),
                            std::memory_order_release);
        }
    }
    // The resident set is captured too: leaving a level must not lose the state of the
    // always-loaded half of it.
    world::CaptureArea(scene, areaId_);
    for (usize i = 0; i < shards_.size(); ++i)
        if (shards_[i]->resident) DespawnShard(scene, static_cast<u32>(i));
}

void Streamer::CaptureAllLoaded(const Scene& scene) {
    if (!bound_) return;
    world::CaptureArea(scene, areaId_);
    for (usize i = 0; i < shards_.size(); ++i) {
        if (!shards_[i]->resident) continue;
        std::vector<entt::entity> members;
        CollectShardEntities(scene, static_cast<u32>(i), members);
        world::CaptureGroup(scene, areaId_, ShardKey(static_cast<u32>(i)), members);
    }
}

// --- Automatic distance streaming ---------------------------------------------

void Streamer::StageJob(void* arg) {
    ShardRuntime* sr = static_cast<ShardRuntime*>(arg);
    Streamer* self = sr->owner;
    salvage::RegionState next = salvage::RegionState::Ready;
    try {
        // SALVAGE 3's correction, applied at the source: stage into a CLEARED buffer.
        // The original re-parsed into a reused, never-cleared scratch, so a retry
        // appended to the previous attempt's rows.
        sr->staged = scene::StagedAssets{};
        scene::StageAssets(self->source_, self->assetsDir_, sr->staged, SlicePtr(sr->rows),
                           static_cast<u32>(sr->rows.size()));
    } catch (const std::exception& e) {
        // A corrupt `.hbmat`/`.uaf` under a shard must not take the process down on a
        // worker thread. Fail the shard; the main thread reports it once and holds it
        // terminal.
        HBE_ERROR("TagStream: staging '{}' threw: {}", sr->desc.tag, e.what());
        sr->staged = scene::StagedAssets{};
        next = salvage::RegionState::Failed;
    } catch (...) {
        sr->staged = scene::StagedAssets{};
        next = salvage::RegionState::Failed;
    }
    // Publish the result BEFORE releasing the in-flight slot, so a drain that observes
    // zero in flight also observes every state.
    sr->state.store(static_cast<int>(next), std::memory_order_release);
    self->inFlight_.fetch_sub(1, std::memory_order_release);
}

void Streamer::Finalize(Scene& scene, Renderer& renderer, u32 i) {
    ShardRuntime& sr = *shards_[i];
    std::vector<entt::entity> created;
    scene::Instantiate(scene, renderer, source_, sr.staged, scene::LoadMode::Additive, &created,
                       /*sceneTag*/ {}, SlicePtr(sr.rows), static_cast<u32>(sr.rows.size()));
    entt::registry& reg = scene.Registry();
    for (const entt::entity e : created)
        if (reg.valid(e)) reg.emplace_or_replace<StreamShard>(e, StreamShard{i});
    // Replay the captured deltas the same frame the entities appear, so nothing ever
    // observes the authored state of a shard the player already changed.
    world::RestoreGroup(scene, areaId_, ShardKey(i), created);
    sr.lastCreated = std::move(created);
    sr.resident = true;
    sr.state.store(static_cast<int>(salvage::RegionState::Loaded), std::memory_order_release);
    // Free the CPU payload now: see ShardRuntime::staged.
    sr.staged = scene::StagedAssets{};
    ++stats_.spawns;
    stats_.residentPeak = glm::max(stats_.residentPeak, ResidentShardCount());
    PublishResidency(scene);
    HBE_TRACE("TagStream: spawned shard '{}' ({} rows, {} entities).", ShardKey(i),
             sr.rows.size(), sr.lastCreated.size());
}

f32 Streamer::NearestFocusDistance(u32 shard, const std::vector<glm::vec3>& foci) const {
    f32 best = std::numeric_limits<f32>::max();
    for (const glm::vec3& f : foci)
        best = glm::min(best, tagshard::DistanceToShard(shards_[shard]->desc, f));
    return best;
}

void Streamer::Update(Scene& scene, Renderer& renderer, const std::vector<glm::vec3>& foci) {
    if (!bound_) return;
    ++frame_;
    ++stats_.framesUpdated;
    // SALVAGE 3, verbatim in shape: at most ONE finalize per frame. Constructed fresh
    // here, so "this frame's budget" needs no reset call and cannot leak across frames.
    salvage::FinalizeBudget finalizeBudget;
    stats_.lastEvalMs = 0.0;
    stats_.lastStructuralMs = 0.0;
    if (shards_.empty()) return; // a level with no streaming tags costs one branch

    using clock = std::chrono::high_resolution_clock;
    const auto timeStructural = [&](auto&& fn) {
        const auto t0 = clock::now();
        fn();
        const f64 ms = std::chrono::duration<f64, std::milli>(clock::now() - t0).count();
        stats_.lastStructuralMs += ms;
        stats_.totalStructuralMs += ms;
        stats_.maxStructuralMs = glm::max(stats_.maxStructuralMs, ms);
        stats_.winMaxStructuralMs = glm::max(stats_.winMaxStructuralMs, ms);
    };

    // --- 1) Finalize completed loads / reap failures (SALVAGE 3) --------------
    // At most ONE finalize per frame: Finalize runs scene::Instantiate synchronously,
    // so finalizing every shard that became ready in the same frame stacks into one
    // main-thread stall - the measured ~1-2 s streaming jank the salvaged comment
    // records. Loads still trickle in through the concurrency throttle, so one per
    // frame keeps up, and IsSettled's "Ready is not settled" clause is what makes the
    // deferral safe (the loading screen holds).
    //
    // THE BUDGET GOES TO THE BEST CANDIDATE, NOT THE LOWEST SHARD INDEX. Up to
    // kMaxConcurrentLoads shards can sit in Ready at once, and the policy already sorted
    // the loads it kicked by priority then nearest distance. Spending the budget on the
    // first Ready shard found by an index scan throws that ordering away: the
    // high-priority shard the author raised precisely so it would land first (enemies in
    // front of the player) can finalize three frames after low-priority debris the
    // player is walking away from - ~100 ms of missing content at 30 FPS, which is the
    // pop the priority field exists to prevent. Same comparator as
    // stream::Evaluate: highest priority, then nearest focus.
    i32 best = -1;
    i32 bestPriority = 0;
    f32 bestDist = 0.0f;
    for (usize i = 0; i < shards_.size(); ++i) {
        ShardRuntime& sr = *shards_[i];
        const auto s = static_cast<salvage::RegionState>(sr.state.load(std::memory_order_acquire));
        if (s == salvage::RegionState::Ready) {
            const f32 d = NearestFocusDistance(static_cast<u32>(i), foci);
            if (best < 0 || sr.priority > bestPriority ||
                (sr.priority == bestPriority && d < bestDist)) {
                best = static_cast<i32>(i);
                bestPriority = sr.priority;
                bestDist = d;
            }
        } else if (s == salvage::RegionState::Failed && !sr.failWarned) {
            sr.failWarned = true;
            ++stats_.failures;
            sr.staged = scene::StagedAssets{};
            // TERMINAL, not reset to Unloaded. See the header: the original's reset
            // re-entered Loading the same Update, so a durably broken shard retried
            // every frame for as long as the player stood near it.
            HBE_TRACE("TagStream: shard '{}' failed to stage; it stays unloaded for this "
                      "session (retrying every frame is worse than a hole).",
                      ShardKey(static_cast<u32>(i)));
        }
    }
    if (best >= 0 && finalizeBudget.Take()) {
        const u32 idx = static_cast<u32>(best);
        timeStructural([&] { Finalize(scene, renderer, idx); });
    }
    // DISTINCT SHARDS, not frame x shard. Counting once per Ready shard per missed frame
    // made 4 shards ready together read as 3+2+1 = 6 deferrals for 4 shards, so the
    // number could not distinguish "four shards each waited once" from "one shard waited
    // four frames" - which is exactly the question it is printed to answer.
    for (usize i = 0; i < shards_.size(); ++i) {
        ShardRuntime& sr = *shards_[i];
        const auto s = static_cast<salvage::RegionState>(sr.state.load(std::memory_order_acquire));
        if (s != salvage::RegionState::Ready) {
            sr.deferred = false;
            continue;
        }
        if (sr.deferred) continue;
        sr.deferred = true;
        ++stats_.deferredFinalizes; // this shard is waiting for a later frame, on purpose
    }

    // --- 2) Cadence ----------------------------------------------------------
    bool due = forceEval_ || lastEvalFrame_ == 0;
    if (!due && (frame_ - lastEvalFrame_) >= kEvalFrameInterval) due = true;
    if (!due && foci.size() == lastEvalFoci_.size()) {
        for (usize i = 0; i < foci.size(); ++i)
            if (glm::distance(foci[i], lastEvalFoci_[i]) >= kEvalMoveDist) {
                due = true;
                break;
            }
    } else if (!due) {
        due = true; // the focus SET changed (a cutscene camera appeared / the player spawned)
    }
    if (!due) return;

    // --- 3) Evaluate (pure; O(shards x foci)) --------------------------------
    const auto t0 = clock::now();
    // PIN A SHARD WHOSE LIVE POPULATION HAS WALKED OUT OF ITS BOX. The policy measures
    // the shard's BAKED AABB, but a spawned NPC inherits its spawner's StreamShard and
    // then MOVES: a guard who bursts from a camp and chases the player 200 m away is
    // still a member of a shard whose box the player left long ago, so the unload would
    // destroy him mid-combat, standing next to the player. His Health/AIBehavior are not
    // persisted either (Spawned is deliberately excluded from world::Persistable), so
    // the fight would simply evaporate. A shard with a live member inside a focus's LOAD
    // radius is therefore pinned. O(spawned x foci), only on an evaluation frame, and
    // exactly zero in a level with no spawned entities.
    pinnedByMember_.assign(shards_.size(), 0u);
    {
        const entt::registry& reg = scene.Registry();
        for (const entt::entity e : reg.view<const Spawned, const StreamShard>()) {
            const u32 idx = reg.get<const StreamShard>(e).index;
            if (idx >= shards_.size() || pinnedByMember_[idx]) continue;
            const glm::vec3 p = glm::vec3(scene.WorldMatrix(e)[3]);
            for (const glm::vec3& f : foci) {
                if (glm::distance(p, f) <= shards_[idx]->loadRadius) {
                    pinnedByMember_[idx] = 1u;
                    break;
                }
            }
        }
    }
    policyScratch_.resize(shards_.size());
    for (usize i = 0; i < shards_.size(); ++i) {
        const ShardRuntime& sr = *shards_[i];
        const auto s = static_cast<salvage::RegionState>(sr.state.load(std::memory_order_acquire));
        PolicyShard& p = policyScratch_[i];
        p.min = sr.desc.min;
        p.max = sr.desc.max;
        p.loadRadius = sr.loadRadius;
        p.unloadRadius = sr.unloadRadius;
        p.priority = sr.priority;
        p.resident = sr.resident;
        p.busy = (s == salvage::RegionState::Loading || s == salvage::RegionState::Ready);
        p.pinned = pinnedByMember_[i] != 0u;
        p.failed = (s == salvage::RegionState::Failed);
    }
    PolicyIn in;
    in.shards = policyScratch_.data();
    in.count = static_cast<u32>(policyScratch_.size());
    in.foci = foci.data();
    in.fociCount = static_cast<u32>(foci.size());
    in.maxConcurrent = kMaxConcurrentLoads;
    in.inFlight = inFlight_.load(std::memory_order_acquire);
    in.maxUnloads = 1;
    in.enabled = enabled_;
    Evaluate(in, policyOut_);
    const f64 evalMs = std::chrono::duration<f64, std::milli>(clock::now() - t0).count();
    stats_.lastEvalMs = evalMs;
    stats_.totalEvalMs += evalMs;
    stats_.maxEvalMs = glm::max(stats_.maxEvalMs, evalMs);
    stats_.winMaxEvalMs = glm::max(stats_.winMaxEvalMs, evalMs);
    ++stats_.evaluations;
    lastEvalFrame_ = frame_;
    lastEvalFoci_ = foci;
    // Re-evaluate next frame while there is throttled work, so a backlog drains at one
    // item per FRAME rather than one per cadence period. That backlog is the normal
    // case (a loading screen ending, a corner turned) and is exactly what the budget
    // exists to spread out - not to slow down.
    forceEval_ = policyOut_.moreWork;

    // --- 4) Act --------------------------------------------------------------
    for (const u32 i : policyOut_.load) {
        ShardRuntime& sr = *shards_[i];
        sr.state.store(static_cast<int>(salvage::RegionState::Loading), std::memory_order_release);
        if (jobs::IsInitialized()) {
            ++stats_.asyncStages;
            inFlight_.fetch_add(1, std::memory_order_acquire);
            jobs::RunDetached(&Streamer::StageJob, &sr, jobs::Priority::Normal);
        } else {
            // No job system (a headless self-test, a tool): stage inline and let the
            // SAME finalize path pick it up next frame. Doing the finalize here too
            // would bypass the one-per-frame budget the whole design rests on.
            ++stats_.syncStages;
            inFlight_.fetch_add(1, std::memory_order_acquire);
            StageJob(&sr);
        }
    }
    for (const u32 i : policyOut_.unload) {
        const u32 idx = i;
        timeStructural([&] { DespawnShard(scene, idx); });
    }
}

bool Streamer::IsSettled(const std::vector<glm::vec3>& foci) const {
    if (!bound_) return true;
    for (usize i = 0; i < shards_.size(); ++i) {
        const auto s =
            static_cast<salvage::RegionState>(shards_[i]->state.load(std::memory_order_acquire));
        // "In range" is the same distance-to-BOX test the policy uses; with no focus at
        // all nothing is in range, which is right - there is nothing to wait for.
        const bool inRange =
            !foci.empty() && NearestFocusDistance(static_cast<u32>(i), foci) <= shards_[i]->loadRadius;
        if (!salvage::RegionSettled(s, inRange)) return false;
    }
    return true;
}

Residency Streamer::QueryGuid(u64 guid) const {
    if (!bound_ || guid == 0) return Residency::NotInLevel;
    const auto it = guidToShard_.find(guid);
    if (it == guidToShard_.end()) return Residency::NotInLevel;
    if (it->second < 0) return Residency::AlwaysLoaded;
    return shards_[static_cast<usize>(it->second)]->resident ? Residency::Resident
                                                             : Residency::StreamedOut;
}

i32 Streamer::ShardOfGuid(u64 guid) const {
    if (!bound_ || guid == 0) return -1;
    const auto it = guidToShard_.find(guid);
    return it == guidToShard_.end() ? -1 : it->second;
}

Residency Streamer::QueryName(const std::string& name) const {
    if (!bound_ || name.empty()) return Residency::NotInLevel;
    const auto it = nameToRow_.find(name);
    if (it == nameToRow_.end()) return Residency::NotInLevel;
    const i32 s = rowToShard_[it->second];
    if (s < 0) return Residency::AlwaysLoaded;
    return shards_[static_cast<usize>(s)]->resident ? Residency::Resident : Residency::StreamedOut;
}

// --- Self-test ----------------------------------------------------------------
namespace {

namespace fs = std::filesystem;

// LIVE entities. Not Scene::EntityCount(), which reports the entity storage's size
// (released slots included) - after a despawn that would count recycled slots and
// hide exactly the leak this test is looking for.
usize LiveCount(const Scene& s) {
    const auto& reg = s.Registry();
    const auto* st = reg.storage<entt::entity>();
    if (!st) return 0;
    usize n = 0;
    for (const entt::entity e : *st)
        if (reg.valid(e)) ++n;
    return n;
}

entt::entity ByGuid(const Scene& s, u64 g) {
    const auto& reg = s.Registry();
    for (const entt::entity e : reg.view<const Guid>())
        if (reg.get<const Guid>(e).value == g) return e;
    return entt::null;
}

// Every raw entt::entity field in the engine, audited across the WHOLE registry.
// A dangling one is not merely wrong: try_get on an invalid handle is an
// ENTT_ASSERT in Debug and an out-of-bounds sparse-set index in Release.
struct HandleAudit {
    usize checked = 0;
    usize dangling = 0;
    std::string first;
};
HandleAudit AuditHandles(const Scene& s) {
    HandleAudit a;
    const auto& reg = s.Registry();
    const auto check = [&](entt::entity holder, entt::entity ref, const char* field) {
        if (ref == entt::null) return;
        ++a.checked;
        if (reg.valid(ref)) return;
        ++a.dangling;
        if (a.first.empty()) {
            const Name* n = reg.try_get<const Name>(holder);
            a.first = std::string(field) + " on '" + (n ? n->value : std::string("<unnamed>")) + "'";
        }
    };
    for (const entt::entity e : reg.view<const Parent>())
        check(e, reg.get<const Parent>(e).entity, "Parent::entity");
    for (const entt::entity e : reg.view<const Health>())
        check(e, reg.get<const Health>(e).lastAttacker, "Health::lastAttacker");
    for (const entt::entity e : reg.view<const AIPerception>())
        check(e, reg.get<const AIPerception>(e).knownTarget, "AIPerception::knownTarget");
    for (const entt::entity e : reg.view<const Destructible>())
        for (const entt::entity ce : reg.get<const Destructible>(e).chunkEntity)
            check(e, ce, "Destructible::chunkEntity");
    for (const entt::entity e : reg.view<const DebrisChunk>())
        check(e, reg.get<const DebrisChunk>(e).owner, "DebrisChunk::owner");
    for (const entt::entity e : reg.view<const Character>())
        for (const auto& [slot, part] : reg.get<const Character>(e).liveParts)
            check(e, part, "Character::liveParts");
    for (const entt::entity e : reg.view<const SkinnedPartRef>())
        check(e, reg.get<const SkinnedPartRef>(e).character, "SkinnedPartRef::character");
    for (const entt::entity e : reg.view<const UICanvas>())
        check(e, reg.get<const UICanvas>(e).surface, "UICanvas::surface");
    for (const entt::entity e : reg.view<const UISurface>())
        check(e, reg.get<const UISurface>(e).canvas, "UISurface::canvas");
    return a;
}

// Total persisted rows for an area, across every shard: the drift/leak pin. Two
// despawn/respawn cycles must not grow it.
usize StoredRows(const std::string& area) {
    const world::AreaState* a = world::Get().Find(area);
    if (!a) return 0;
    usize n = 0;
    for (const auto& [key, sh] : a->shards) n += sh.present.size() + sh.blobs.size();
    return n;
}

} // namespace

bool SelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("shardstate: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_shardstate";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "Camp.hbscene";

    // The project's tag list. "Ground" is alwaysLoaded, which must join the RESIDENT
    // slice even though the bake gives it a shard of its own.
    std::vector<TagDef> defs;
    {
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        TagDef camp;
        camp.name = "Camp";
        camp.loadRadius = 50.0f;
        TagDef farTag; // not `far`: windows.h still #defines that legacy keyword
        farTag.name = "Far";
        farTag.loadRadius = 50.0f;
        TagDef ground;
        ground.name = "Ground";
        ground.alwaysLoaded = true;
        defs = {untagged, camp, farTag, ground};
        tags::Normalize(defs);
        tags::SeedFromProject(defs);
    }
    const TagId tCamp = tags::Intern("Camp"), tFar = tags::Intern("Far"),
                tGround = tags::Intern("Ground");

    // Guids of the authored entities, captured while authoring so every later
    // assertion addresses entities the way PERSISTENCE does - by guid, never by name.
    u64 gDoor = 0, gPlayer = 0, gGround = 0, gHut = 0, gRoof = 0, gGuard = 0, gCrateA = 0,
        gCrateB = 0, gTrap = 0, gSpawner = 0, gEncounter = 0, gPickup = 0, gWatcher = 0;

    {
        Scene s;
        auto& reg = s.Registry();
        const auto make = [&](const char* n, const glm::vec3& p, TagId tag) {
            const entt::entity e = s.CreateEntity(n);
            Transform t;
            t.position = p;
            reg.emplace<Transform>(e, t);
            reg.emplace<AABB>(e, AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});
            if (tag != kTagUntagged) tags::Assign(reg, e, tag);
            return e;
        };

        // --- Always resident (untagged + an alwaysLoaded tag) ------------------
        const entt::entity door = make("Door", {0.0f, 0.0f, 0.0f}, kTagUntagged);
        {
            Interactable ia;
            ia.prompt = "Open";
            ia.once = true;
            reg.emplace<Interactable>(door, ia);
        }
        const entt::entity player = make("Player", {0.0f, 0.0f, 2.0f}, kTagUntagged);
        reg.emplace<CharacterController>(player, CharacterController{});
        reg.emplace<Health>(player, Health{});
        const entt::entity ground = make("GroundPlane", {0.0f, -1.0f, 0.0f}, tGround);

        // --- Tag "Camp" around x=100 ------------------------------------------
        const entt::entity hut = make("Hut", {100.0f, 0.0f, 0.0f}, tCamp);
        const entt::entity roof = make("HutRoof", {0.0f, 3.0f, 0.0f}, tCamp);
        reg.emplace<Parent>(roof, Parent{hut});

        const entt::entity guard = make("Guard", {102.0f, 0.0f, 0.0f}, tCamp);
        {
            Health h;
            h.max = 100.0f;
            h.current = 100.0f;
            h.onDeathFlag = "guard_dead";
            reg.emplace<Health>(guard, h);
            AIBehavior b;
            b.patrolPoints = {{100.0f, 0.0f, 0.0f}, {110.0f, 0.0f, 0.0f}};
            reg.emplace<AIBehavior>(guard, b);
            reg.emplace<AIPerception>(guard, AIPerception{});
            reg.emplace<NavigationAgent>(guard, NavigationAgent{});
        }

        // TWO ENTITIES WITH THE SAME NAME, both carrying independent break progress.
        // Under the old Name key these collapsed onto one persistence row and
        // overwrote each other; under a guid key they must restore independently.
        // This is the single most direct proof that the key changed.
        const auto crate = [&](const glm::vec3& p) {
            const entt::entity e = make("Crate", p, tCamp);
            Destructible ds;
            ds.asset = "Fracture/Crate.hbfrac";
            ds.chunkState.assign(3, 0);
            ds.chunkHp.assign(3, 12.0f);
            reg.emplace<Destructible>(e, ds);
            return e;
        };
        const entt::entity crateA = crate({104.0f, 0.0f, 0.0f});
        const entt::entity crateB = crate({106.0f, 0.0f, 0.0f});

        const entt::entity trap = make("Trap", {108.0f, 0.0f, 0.0f}, tCamp);
        {
            TriggerVolume tv;
            tv.flag = "camp_entered";
            tv.once = true;
            reg.emplace<TriggerVolume>(trap, tv);
        }
        const entt::entity spawner = make("CampSpawner", {110.0f, 0.0f, 0.0f}, tCamp);
        {
            Spawner sp;
            sp.prefab = "Prefabs/Guard.hbprefab";
            sp.spawnerId = "camp";
            sp.encounterId = "camp";
            sp.trigger = Spawner::Trigger::Manual;
            reg.emplace<Spawner>(spawner, sp);
        }
        const entt::entity encounter = make("CampEncounter", {112.0f, 0.0f, 0.0f}, tCamp);
        {
            Encounter en;
            en.id = "camp";
            en.clearedFlag = "camp_cleared";
            reg.emplace<Encounter>(encounter, en);
        }
        // A looted pickup: destroyed during the visit, so the DIFF (not a flag) has to
        // keep it gone across a respawn.
        const entt::entity pickup = make("Pickup", {114.0f, 0.0f, 0.0f}, tCamp);
        {
            Interactable ia;
            ia.action = InteractAction::GrantItem;
            ia.itemId = "medkit";
            ia.pickupId = "camp_medkit";
            reg.emplace<Interactable>(pickup, ia);
        }

        // --- Tag "Far" around x=600 (holds the CROSS-SHARD reference) ----------
        const entt::entity watcher = make("Watcher", {600.0f, 0.0f, 0.0f}, tFar);
        reg.emplace<Health>(watcher, Health{});
        reg.emplace<AIPerception>(watcher, AIPerception{});

        const auto g = [&](entt::entity e) { return reg.get<Guid>(e).value; };
        gDoor = g(door);
        gPlayer = g(player);
        gGround = g(ground);
        gHut = g(hut);
        gRoof = g(roof);
        gGuard = g(guard);
        gCrateA = g(crateA);
        gCrateB = g(crateB);
        gTrap = g(trap);
        gSpawner = g(spawner);
        gEncounter = g(encounter);
        gPickup = g(pickup);
        gWatcher = g(watcher);

        expect(gCrateA != 0 && gCrateB != 0 && gCrateA != gCrateB,
               "two same-named crates get DIFFERENT guids (the whole point of the new key)");

        const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
        expect(rep.errors == 0, "the authored test scene bakes without errors");
        expect(scene::SaveScene(s, file, {}, SceneKind::Full, &rep.shards),
               "save the authored scene with its baked shard header");
    }

    world::Get().Clear();
    world::SetCurrentArea({});

    // No GPU: a device-less Renderer's UploadMesh/UploadTexture return invalid handles
    // instead of touching a device, the headless contract the other self-tests keep.
    Renderer renderer;
    Scene s;
    Streamer st;
    expect(st.BindLevel(s, renderer, file, dir, defs), "BindLevel parses and binds the level");
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    const std::string area = st.AreaId();
    expect(area == "camp", "the area id is the file stem, lower-cased");
    expect(st.Trusted(), "the freshly baked shard header is trusted");

    // --- 1. Bind leaves streamed shards UNLOADED and always-loaded ones resident --
    expect(st.ShardCount() == 2,
           "exactly two STREAMED shards: the alwaysLoaded 'Ground' tag is resident, not streamed");
    const i32 iCamp = st.FindShard("Camp#0"), iFar = st.FindShard("Far#0");
    expect(iCamp >= 0 && iFar >= 0, "both streamed shards are addressable by '<tag>#<index>'");
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    expect(ByGuid(s, gDoor) != entt::null && ByGuid(s, gPlayer) != entt::null &&
               ByGuid(s, gGround) != entt::null,
           "the resident slice (untagged + alwaysLoaded) instantiated at bind");
    expect(ByGuid(s, gGuard) == entt::null && ByGuid(s, gWatcher) == entt::null,
           "streamed shards created NOTHING at bind");
    expect(st.QueryName("Guard") == Residency::StreamedOut &&
               st.QueryName("Door") == Residency::AlwaysLoaded &&
               st.QueryName("GroundPlane") == Residency::AlwaysLoaded &&
               st.QueryName("Nope") == Residency::NotInLevel,
           "residency answers for an entity that does not exist yet");
    expect(st.QueryGuid(gGuard) == Residency::StreamedOut && st.QueryGuid(0xdeadbeefull) ==
                                                                 Residency::NotInLevel,
           "residency by guid agrees with residency by name");
    const usize nBind = LiveCount(s);

    // --- 2. Spawn both shards ------------------------------------------------
    expect(st.SpawnShard(s, renderer, static_cast<u32>(iFar)), "spawn Far#0");
    const usize nFar = LiveCount(s);
    expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)), "spawn Camp#0");
    const usize nBoth = LiveCount(s);
    expect(nFar > nBind && nBoth > nFar, "each spawn adds entities");
    expect(!st.SpawnShard(s, renderer, static_cast<u32>(iCamp)),
           "spawning an already-resident shard is refused, not doubled");
    expect(LiveCount(s) == nBoth, "the refused spawn created nothing");
    expect(st.QueryName("Guard") == Residency::Resident, "Guard reads Resident once spawned");
    {
        const entt::entity hut = ByGuid(s, gHut), roof = ByGuid(s, gRoof);
        expect(hut != entt::null && roof != entt::null &&
                   s.Registry().get<Parent>(roof).entity == hut,
               "a whole subtree rides its shard, hierarchy intact");
    }

    // --- 3. Play the world: change everything that has to persist ------------
    // Everything below dereferences the spawned entities, so a failure above must not
    // become a null dereference here.
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    entt::entity spawnedNpc = entt::null;
    {
        auto& reg = s.Registry();
        reg.get<Interactable>(ByGuid(s, gDoor)).fired = true; // resident, must be untouched by cycles

        const entt::entity guard = ByGuid(s, gGuard);
        {
            Health& h = reg.get<Health>(guard);
            h.current = 30.0f;
            h.alive = false;
            h.deathDispatched = true;
            AIBehavior& b = reg.get<AIBehavior>(guard);
            b.state = AIState::Chase;
            b.patrolIndex = 2;
            b.patrolForward = false;
            b.spawnApplied = true;
            AIPerception& p = reg.get<AIPerception>(guard);
            p.awareness = 0.8f;
            p.timeSinceSeen = 1.5f;
            reg.get<NavigationAgent>(guard).hasTarget = true;
        }
        // Independent break progress per crate.
        {
            Destructible& a = reg.get<Destructible>(ByGuid(s, gCrateA));
            a.activated = true;
            a.chunkState = {2, 1, 0};
            a.chunkHp = {0.0f, 5.0f, 12.0f};
            Destructible& b = reg.get<Destructible>(ByGuid(s, gCrateB));
            b.activated = true;
            b.chunkState = {0, 0, 2};
            b.chunkHp = {12.0f, 12.0f, 0.0f};
        }
        reg.get<TriggerVolume>(ByGuid(s, gTrap)).fired = true;
        {
            Spawner& sp = reg.get<Spawner>(ByGuid(s, gSpawner));
            sp.activated = true;
            sp.spawnedTotal = 3;
            sp.respawnCooldown = 2.5f;
            sp.inside = true;
        }
        {
            Encounter& en = reg.get<Encounter>(ByGuid(s, gEncounter));
            en.state = Encounter::State::Cleared;
            en.everHadAlive = true;
        }
        // A surviving spawned NPC: it must be DESTROYED with the shard and must NOT
        // come back (spawner progress only - decision 4, and what keeps saves bounded).
        spawnedNpc = s.CreateEntity("Guard_Spawned_0");
        reg.emplace<Transform>(spawnedNpc, Transform{});
        reg.emplace<Health>(spawnedNpc, Health{});
        reg.emplace<Spawned>(spawnedNpc, Spawned{"camp", "camp"});

        // THE CROSS-SHARD HANDLE: a Far entity pointing at a Camp entity. Read today
        // without any validity test (CombatSystem laundering lastAttacker into
        // game::DeathRec::instigator), so a despawn must scrub it.
        reg.get<Health>(ByGuid(s, gWatcher)).lastAttacker = guard;
        reg.get<AIPerception>(ByGuid(s, gWatcher)).knownTarget = guard;

        // A looted pickup: destroyed outright.
        reg.destroy(ByGuid(s, gPickup));
    }
    const usize nPlayed = LiveCount(s); // -1 pickup, +1 spawned NPC
    expect(nPlayed == nBoth, "the played world has the same count (one looted, one spawned)");
    {
        // Sanity-check the AUDIT ITSELF while the references exist: after the despawn
        // below there are legitimately none left to inspect (the cross-shard ones get
        // scrubbed, the Parent link goes with the shard), so "zero dangling" there would
        // otherwise be vacuously true whether or not the audit looks at anything.
        const HandleAudit a = AuditHandles(s);
        expect(a.checked >= 3 && a.dangling == 0,
               "the handle audit sees the live Parent + the two cross-shard references, and "
               "none of them is dangling yet");
    }

    // --- 4. Despawn Camp#0 ---------------------------------------------------
    const u32 npcBits = static_cast<u32>(spawnedNpc);
    expect(st.DespawnShard(s, static_cast<u32>(iCamp)), "despawn Camp#0");
    expect(!st.DespawnShard(s, static_cast<u32>(iCamp)),
           "despawning an already-absent shard is refused");
    expect(LiveCount(s) == nFar,
           "the entity count returns EXACTLY to its pre-Camp value: no leak, and the "
           "spawner's NPC went with the shard");
    expect(!s.Registry().valid(static_cast<entt::entity>(npcBits)),
           "the Spawned NPC was collected by the closure (spawnerId), not left orphaned");
    expect(ByGuid(s, gGuard) == entt::null && ByGuid(s, gCrateA) == entt::null &&
               ByGuid(s, gHut) == entt::null && ByGuid(s, gRoof) == entt::null,
           "every Camp member is gone, subtree included");
    expect(ByGuid(s, gDoor) != entt::null && ByGuid(s, gWatcher) != entt::null,
           "the resident set and the other shard are untouched");
    {
        const HandleAudit a = AuditHandles(s);
        expect(a.dangling == 0, "no surviving component holds a dangling entt::entity");
        if (a.dangling != 0) HBE_ERROR("shardstate: first dangling handle was {}", a.first);
        const auto& reg = s.Registry();
        expect(reg.get<Health>(ByGuid(s, gWatcher)).lastAttacker == entt::null &&
                   reg.get<AIPerception>(ByGuid(s, gWatcher)).knownTarget == entt::null,
               "the cross-shard references were SCRUBBED to null, not left stale");
    }
    expect(st.QueryName("Guard") == Residency::StreamedOut,
           "a despawned entity reads StreamedOut (it will come back), never NotInLevel");

    // A non-resident shard must NOT be diffed as destroyed by a whole-scene capture:
    // that is the data-loss bug an area-wide `present` set would cause.
    world::CaptureArea(s, area);
    // And an EMPTY capture must not strip a real one.
    world::CaptureGroup(s, area, "Camp#0", {});
    {
        const world::ShardState* sh = world::Get().FindShard(area, "Camp#0");
        expect(sh && sh->captured && sh->present.size() >= 6,
               "the empty capture was refused; Camp#0's recorded state survives");
    }

    // --- 5. Respawn and verify EVERY field restored by guid -------------------
    expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)), "respawn Camp#0");
    const usize nRespawn = LiveCount(s);
    expect(nRespawn == nBoth - 1,
           "respawn restores every member EXCEPT the looted pickup, and spawns no NPC");
    {
        const auto& reg = s.Registry();
        const entt::entity guard = ByGuid(s, gGuard);
        expect(guard != entt::null, "the Guard came back");
        if (guard != entt::null) {
            const Health& h = reg.get<const Health>(guard);
            expect(h.current == 30.0f && !h.alive && h.deathDispatched,
                   "Health restored: wounded, dead, and its one-shot death dispatch stays spent");
            expect(!reg.get<const NavigationAgent>(guard).hasTarget,
                   "a restored corpse stops pathing (presentation is applied, effects are not)");
            const AIBehavior& b = reg.get<const AIBehavior>(guard);
            expect(b.state == AIState::Chase && b.patrolIndex == 2 && !b.patrolForward &&
                       b.spawnApplied,
                   "AIBehavior FSM state and patrol progress restored");
            const AIPerception& p = reg.get<const AIPerception>(guard);
            expect(p.awareness == 0.8f && p.timeSinceSeen == 1.5f,
                   "AIPerception awareness restored");
            expect(p.knownTarget == entt::null,
                   "AIPerception::knownTarget is NOT restored - a handle from a previous life");
        }
        const entt::entity ca = ByGuid(s, gCrateA), cb = ByGuid(s, gCrateB);
        expect(ca != entt::null && cb != entt::null, "both same-named crates came back");
        if (ca != entt::null && cb != entt::null) {
            const Destructible& a = reg.get<const Destructible>(ca);
            const Destructible& b = reg.get<const Destructible>(cb);
            expect(a.activated && a.chunkState == std::vector<u8>({2, 1, 0}) &&
                       a.chunkHp == std::vector<f32>({0.0f, 5.0f, 12.0f}),
                   "crate A's break progress restored");
            expect(b.activated && b.chunkState == std::vector<u8>({0, 0, 2}) &&
                       b.chunkHp == std::vector<f32>({12.0f, 12.0f, 0.0f}),
                   "crate B's break progress restored INDEPENDENTLY (guid, not name)");
            expect(a.chunkEntity.size() == 3 && b.chunkEntity.size() == 3,
                   "chunkEntity[] is re-sized index-parallel and holds no stale handles");
            // The RESTORE/REBUILD HANDSHAKE. `activated` with an all-null chunkEntity[]
            // is a state the destruction system cannot represent: Activate() refuses to
            // run on an activated object, and Activate() is also what removes the root's
            // intact MeshInstance/RigidBody - so the wall would come back rendering and
            // colliding as PRISTINE while being internally fully broken and permanently
            // unbreakable. `reactivate` is what tells destruction::Update to rebuild the
            // chunk entities and re-detach the ones that were detached, and
            // structureDirty must stay FALSE because the captured chunkState is already
            // post-solve. (The rebuild itself needs a Renderer + a real `.hbfrac`, so it
            // is not reachable from this headless test; the flag half is.)
            expect(a.reactivate && b.reactivate,
                   "a restored break is flagged for chunk-entity REBUILD, not left as "
                   "'activated with no chunks' (a pristine-looking unbreakable wall)");
            expect(!a.structureDirty && !b.structureDirty,
                   "and the support solve is NOT re-owed - the restored state is already "
                   "solved, so re-solving would cascade it to Detached invisibly");
        }
        expect(reg.get<const TriggerVolume>(ByGuid(s, gTrap)).fired &&
                   !reg.get<const TriggerVolume>(ByGuid(s, gTrap)).inside,
               "a fired one-shot trigger stays fired, with its enter edge re-armed");
        const Spawner& sp = reg.get<const Spawner>(ByGuid(s, gSpawner));
        expect(sp.activated && sp.spawnedTotal == 3 && sp.respawnCooldown == 2.5f && !sp.inside,
               "SPAWNER PROGRESS survives (activated + lifetime total + cooldown)");
        const Encounter& en = reg.get<const Encounter>(ByGuid(s, gEncounter));
        expect(en.state == Encounter::State::Cleared && en.everHadAlive && en.aliveCount == 0 &&
                   !en.clearedEdge,
               "a CLEARED encounter stays cleared and does not re-fire its cleared action");
        expect(ByGuid(s, gPickup) == entt::null, "the looted pickup stays looted");
        usize spawned = 0;
        for (const entt::entity e : reg.view<const Spawned>()) {
            (void)e;
            ++spawned;
        }
        expect(spawned == 0, "the spawned POPULATION is deliberately not persisted");
        expect(reg.get<const Interactable>(ByGuid(s, gDoor)).fired,
               "the resident set's state is untouched by a shard cycle");
        const HandleAudit a = AuditHandles(s);
        expect(a.dangling == 0, "the respawned world holds no dangling handle either");
    }

    // --- 6. A second full cycle is stable (no drift, no leak) ----------------
    const usize rows1 = StoredRows(area);
    expect(st.DespawnShard(s, static_cast<u32>(iCamp)), "second despawn");
    expect(LiveCount(s) == nFar, "second despawn returns to the same baseline");
    expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)), "second respawn");
    expect(LiveCount(s) == nRespawn, "second respawn produces the SAME entity count");
    expect(StoredRows(area) == rows1, "the persisted row count does not grow per cycle");
    {
        const auto& reg = s.Registry();
        const Health& h = reg.get<const Health>(ByGuid(s, gGuard));
        expect(h.current == 30.0f && !h.alive, "state survives two cycles without drifting");
        expect(reg.get<const Destructible>(ByGuid(s, gCrateB)).chunkState ==
                   std::vector<u8>({0, 0, 2}),
               "per-crate break progress survives two cycles");
        expect(AuditHandles(s).dangling == 0, "no dangling handle after two cycles");
    }

    // --- 7. The raw-bits guard, PINNED ON THE PRODUCTION ONE -----------------
    // Invariant 3's last clause: a reference that cannot be scrubbed - a schematic
    // Entity pin, a game::DeathRec instigator - is raw u32 handle bits, and
    // entt::try_get on a stale handle is an ENTT_ASSERT in Debug / an out-of-bounds
    // sparse-set read in Release. Despawn makes that routine. The guard is
    // schematic::BakedEnt/BakedGet, at the one place both the interpreter and the
    // transpiled C++ resolve a pin; it lives in the Schematic layer because that layer
    // cannot depend on Scene. (This used to test a Scene-side SafeEntity() with zero
    // production call sites - i.e. the invariant was pinned on dead code.)
    {
        auto& reg = s.Registry();
        const entt::entity live = ByGuid(s, gDoor);
        const entt::entity dead = s.CreateEntity("Doomed");
        const u32 deadBits = static_cast<u32>(dead);
        reg.destroy(dead);
        const auto pin = [](u32 bits) {
            schematic::Value v;
            v.type = schematic::PinType::Entity;
            v.entity = bits;
            return v;
        };
        expect(schematic::BakedEnt(reg, pin(static_cast<u32>(live)), entt::null) == live,
               "a schematic Entity pin holding a live handle resolves to it");
        expect(schematic::BakedEnt(reg, pin(deadBits), entt::null) == entt::null,
               "a pin holding a DESTROYED handle resolves to null, not a sparse-set read");
        expect(schematic::BakedEnt(reg, pin(0xFFFFFFFFu), live) == live,
               "the unset sentinel falls back to `self`");
        expect(schematic::BakedGet<Health>(reg, entt::null) == nullptr,
               "BakedGet tolerates a null handle instead of asserting");
    }

    // --- 8. The save blob round-trips, and a v1 save degrades honestly -------
    {
        st.CaptureAllLoaded(s);
        const std::string text = world::Get().Serialize();
        // Despawn FIRST, then wipe the in-memory state and reload it from the serialized
        // bytes, so the respawn below has no choice but to restore from the save.
        expect(st.DespawnShard(s, static_cast<u32>(iCamp)), "despawn before the save round-trip");
        world::Get().Clear();
        expect(world::Get().FindShard(area, "Camp#0") == nullptr, "cleared state is really gone");
        world::Get().Deserialize(text);
        const world::ShardState* sh = world::Get().FindShard(area, "Camp#0");
        expect(sh && sh->captured && sh->blobs.count(gGuard) == 1 &&
                   sh->blobs.count(gCrateA) == 1 && sh->blobs.count(gCrateB) == 1,
               "guid-keyed blobs survive serialize -> deserialize");
        expect(world::Get().VisitCount(area) == 1, "the visit count round-trips");
        expect(st.SpawnShard(s, renderer, static_cast<u32>(iCamp)),
               "respawn against nothing but the deserialized state");
        const entt::entity guard = ByGuid(s, gGuard);
        expect(guard != entt::null &&
                   s.Registry().get<const Health>(guard).current == 30.0f &&
                   !s.Registry().get<const Health>(guard).alive,
               "state restored from a DESERIALIZED save, not from memory");
        expect(ByGuid(s, gPickup) == entt::null,
               "and the destroyed-set diff survives the save round-trip too");
        expect(LiveCount(s) == nRespawn, "the round-tripped restore rebuilds the same world");
    }
    {
        // Format v1: name keys + five bools, no "version". visits/vars must survive;
        // the per-entity rows cannot be mapped onto guids and are dropped, loudly.
        world::Get().Clear();
        world::Get().Deserialize(R"({"areas":{"oldarea":{"visits":7,"captured":true,)"
                                 R"("present":["Door"],"vars":{"alarm":3.5},)"
                                 R"("entities":{"Door":{"interacted":true}}}}})");
        expect(world::Get().VisitCount("oldarea") == 7, "a v1 save keeps its visit count");
        expect(world::Get().GetVar("oldarea", "alarm") == 3.5f, "a v1 save keeps its area vars");
        expect(world::Get().FindShard("oldarea", world::kResidentKey) == nullptr,
               "a v1 save's name-keyed entity rows are dropped, not mis-keyed onto guids");
    }

    // --- 9. Teardown --------------------------------------------------------
    {
        world::Get().Clear();
        st.UnloadAll(s);
        expect(!st.IsResident(static_cast<u32>(iCamp)) && !st.IsResident(static_cast<u32>(iFar)),
               "UnloadAll despawns every resident shard");
        expect(world::Get().FindShard(area, "Camp#0") != nullptr &&
                   world::Get().FindShard(area, world::kResidentKey) != nullptr,
               "UnloadAll captures the shards AND the always-resident set");
        expect(AuditHandles(s).dangling == 0, "teardown leaves no dangling handle");
    }

    // --- 9b. Rebinding the same level does not stack a second world ----------
    {
        const u32 visitsBefore = world::Get().VisitCount(area);
        expect(st.BindLevel(s, renderer, file, dir, defs), "rebind the level");
        expect(LiveCount(s) == nBind,
               "a rebind REPLACES the world (BindWorld) instead of stacking a second copy");
        expect(world::Get().VisitCount(area) == visitsBefore + 1,
               "entering the area again bumps the visit count exactly once (per ENTRY, never "
               "per shard spawn)");
        expect(s.Registry().get<const Interactable>(ByGuid(s, gDoor)).fired,
               "the resident set's state was captured on the way out and replayed on the way in");
        expect(ByGuid(s, gGuard) == entt::null, "and the streamed shards start unloaded again");
        expect(!st.BindLevel(s, renderer, dir / "NoSuchFile.hbscene", dir, defs),
               "binding a file that does not parse fails");
        expect(st.IsBound() && st.AreaId() == area && LiveCount(s) == nBind,
               "and leaves the existing binding and its world completely untouched");
    }

    // --- 10. An UNTRUSTED shard header degrades to everything-resident -------
    {
        // Hand-corrupt the header's member count: correct-but-unstreamed is the only
        // acceptable degradation, never missing content.
        std::string text;
        {
            std::ifstream in(file, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        expect(j.is_object() && j.contains("tagShards"), "the saved file carries a shard header");
        if (j.is_object() && j.contains("tagShards") && !j["tagShards"].empty()) {
            j["tagShards"][0]["count"] = 999u;
            const fs::path bad = dir / "Bad.hbscene";
            {
                std::ofstream out(bad, std::ios::binary | std::ios::trunc);
                const std::string s2 = j.dump();
                out.write(s2.data(), static_cast<std::streamsize>(s2.size()));
            }
            Scene s3;
            Streamer st3;
            world::Get().Clear();
            expect(st3.BindLevel(s3, renderer, bad, dir, defs), "a stale header still binds");
            expect(!st3.Trusted() && st3.ShardCount() == 0,
                   "an untrusted shard table yields ZERO streamed shards");
            expect(ByGuid(s3, gGuard) != entt::null && ByGuid(s3, gWatcher) != entt::null &&
                       ByGuid(s3, gDoor) != entt::null,
                   "and instead loads EVERYTHING resident - unstreamed, never missing");
        }
    }

    // --- 11. AUTOMATIC distance streaming (P6) -------------------------------
    // Radii come from the TagDef at bind time, so a wider band is a different defs
    // vector rather than a different scene: with 400 m radii BOTH shards fall in range
    // of one focus, which is what makes the one-finalize-per-frame budget observable.
    {
        std::vector<TagDef> wide = defs;
        for (TagDef& d : wide)
            if (d.name == "Camp" || d.name == "Far") d.loadRadius = 400.0f;
        tags::Normalize(wide); // re-derives the hysteresis band for the new radius

        world::Get().Clear();
        Scene s2;
        Streamer st2;
        expect(st2.BindLevel(s2, renderer, file, dir, wide), "bind for the automatic-streaming pass");
        const usize base = LiveCount(s2);
        const i32 c2 = st2.FindShard("Camp#0"), f2 = st2.FindShard("Far#0");
        expect(c2 >= 0 && f2 >= 0, "both shards are addressable in the wide binding");

        // A focus 100 km away: in range of nothing, so nothing loads however many
        // frames pass.
        std::vector<glm::vec3> foci{glm::vec3(100000.0f, 0.0f, 0.0f)};
        for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 0 && LiveCount(s2) == base,
               "a distant focus loads nothing, ever");
        expect(st2.IsSettled(foci), "and with nothing in range the world is SETTLED");

        // Move the focus so both shards are in range. Update 1 evaluates and STAGES;
        // it must not also finalize (that is what the budget is).
        foci[0] = glm::vec3(350.0f, 0.0f, 0.0f);
        st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 0,
               "the evaluation that orders a load does not also instantiate it");
        expect(!st2.IsSettled(foci),
               "a staged-but-not-instantiated shard is NOT settled (SALVAGE 4's Ready "
               "clause - dropping the loading screen here is the pop it exists to hide)");
        // Update 2: exactly ONE finalize, and the other is deferred - the salvaged
        // budget, and the whole reason a corner-turn does not stall for a second.
        st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 1,
               "ONE shard finalizes per frame even when two are ready (SALVAGE 3)");
        expect(st2.Stats().deferredFinalizes >= 1, "and the deferral is counted, not silent");
        expect(!st2.IsSettled(foci), "still not settled with one shard outstanding");
        st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 2, "the deferred shard finalizes on the next frame");
        expect(st2.IsSettled(foci), "with everything in range resident, the world is settled");
        const usize both2 = LiveCount(s2);
        expect(both2 > base, "the automatic spawns really created entities");

        // Walk away: both shards unload. The unload cap is one per evaluation, and
        // moreWork forces the next frame to evaluate again, so a backlog drains at one
        // per frame instead of one per cadence period.
        foci[0] = glm::vec3(100000.0f, 0.0f, 0.0f);
        u32 frames = 0;
        while (st2.ResidentShardCount() > 0 && frames < 40) {
            st2.Update(s2, renderer, foci);
            ++frames;
        }
        expect(st2.ResidentShardCount() == 0, "walking away unloads every shard");
        expect(frames <= 4,
               "and the moreWork backlog drains at one per FRAME, not one per cadence period");
        expect(LiveCount(s2) == base,
               "the entity count returns EXACTLY to the post-bind baseline: automatic "
               "streaming leaks nothing");
        expect(st2.Stats().spawns == 2 && st2.Stats().despawns == 2,
               "two automatic spawns and two automatic despawns happened");
        expect(AuditHandles(s2).dangling == 0, "automatic despawn leaves no dangling handle");

        // Hysteresis at the boundary: just OUTSIDE the load radius but inside the
        // unload radius keeps a resident shard resident. Done with only Camp in play,
        // so the assertion is about the band and not about which shard moved.
        {
            std::vector<glm::vec3> at{glm::vec3(105.0f, 0.0f, 0.0f)}; // inside the Camp box
            for (int i = 0; i < 6; ++i) st2.Update(s2, renderer, at);
            expect(st2.IsResident(static_cast<u32>(c2)) && !st2.IsResident(static_cast<u32>(f2)),
                   "standing in the Camp loads Camp and leaves the 600 m-away shard alone");
            const ShardRuntime& sr = st2.Shard(static_cast<u32>(c2));
            expect(sr.unloadRadius > sr.loadRadius, "the band is non-degenerate after Normalize");
            const f32 mid = (sr.loadRadius + sr.unloadRadius) * 0.5f;
            std::vector<glm::vec3> band{glm::vec3(sr.desc.min.x - mid, 0.0f, 0.0f)};
            for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, band);
            expect(st2.IsResident(static_cast<u32>(c2)),
                   "a focus in the DEAD BAND (outside load, inside unload) does not unload");
            // And one step past the unload radius does.
            std::vector<glm::vec3> past{
                glm::vec3(sr.desc.min.x - sr.unloadRadius - 5.0f, 0.0f, 0.0f)};
            for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, past);
            expect(!st2.IsResident(static_cast<u32>(c2)), "one step past the unload radius does");
        }

        // A second full sweep is stable - the drift pin, automatic edition.
        foci[0] = glm::vec3(350.0f, 0.0f, 0.0f);
        for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 2 && LiveCount(s2) == both2,
               "a second sweep in rebuilds the SAME world");
        foci[0] = glm::vec3(100000.0f, 0.0f, 0.0f);
        for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, foci);
        expect(LiveCount(s2) == base, "and a second sweep out returns to the same baseline");

        // SetEnabled(false) pins everything loaded, wherever the focus is. "Streaming
        // off" must mean the whole level is there, never "half of it is missing".
        st2.SetEnabled(false);
        for (int i = 0; i < 10; ++i) st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 2,
               "streaming DISABLED loads every shard even with the focus 100 km away");
        for (int i = 0; i < 10; ++i) st2.Update(s2, renderer, foci);
        expect(st2.ResidentShardCount() == 2, "and never unloads any of them");

        // Cost: a stationary focus with nothing to do must not evaluate every frame.
        // (Still disabled, so the world stays put and only the cadence is measured.)
        const u32 evalsBefore = st2.Stats().evaluations;
        for (int i = 0; i < 40; ++i) st2.Update(s2, renderer, foci);
        const u32 evals = st2.Stats().evaluations - evalsBefore;
        expect(evals > 0 && evals <= 40 / kEvalFrameInterval + 2,
               "a stationary focus evaluates on the CADENCE, not every frame");
        st2.SetEnabled(true);

        // The ASYNC path. Everything above ran the synchronous fallback, because the
        // headless test binary has no job system - so start one and prove the worker
        // path lands too (StageAssets on a worker, Instantiate on this thread).
        expect(st2.Stats().asyncStages == 0,
               "without a job system every stage took the synchronous fallback");
        {
            st2.UnloadAll(s2);
            jobs::Initialize();
            st2.ResetStats();
            foci[0] = glm::vec3(350.0f, 0.0f, 0.0f);
            u32 spin = 0;
            while (st2.ResidentShardCount() < 2 && spin < 2000) {
                st2.Update(s2, renderer, foci);
                ++spin;
            }
            expect(st2.ResidentShardCount() == 2, "both shards load through the JOB SYSTEM");
            expect(st2.Stats().asyncStages == 2 && st2.Stats().syncStages == 0,
                   "and they really went through jobs::RunDetached, not the fallback");
            expect(LiveCount(s2) == both2, "the async path builds the same world");
            expect(st2.IsSettled(foci), "and settles");
            // Reset while a load is in flight: the drain is what stops the job writing
            // into freed shards (plan blocker B13's use-after-free).
            foci[0] = glm::vec3(100000.0f, 0.0f, 0.0f);
            for (int i = 0; i < 8; ++i) st2.Update(s2, renderer, foci);
            foci[0] = glm::vec3(350.0f, 0.0f, 0.0f);
            st2.Update(s2, renderer, foci); // kicks jobs, does not finalize
            st2.Reset();                    // drains them
            expect(!st2.IsBound(), "Reset with loads in flight drains and unbinds cleanly");
            jobs::Shutdown();
        }
    }

    // --- 12. Save/load residency (P7) ----------------------------------------
    {
        world::Get().Clear();
        Scene s3;
        Streamer st3;
        expect(st3.BindLevel(s3, renderer, file, dir, defs), "bind for the save/load pass");
        const i32 c3 = st3.FindShard("Camp#0");
        expect(c3 >= 0 && st3.SpawnShard(s3, renderer, static_cast<u32>(c3)),
               "spawn one shard, so the save has a resident shard to record");
        // Change something inside the shard so "restored" means restored, not re-authored.
        s3.Registry().get<Health>(ByGuid(s3, gGuard)).current = 7.0f;
        st3.CaptureAllLoaded(s3);
        const std::vector<std::string> keys = st3.ResidentKeys();
        expect(keys.size() == 1 && keys[0] == "Camp#0",
               "ResidentKeys reports exactly the resident shard, sorted");
        const std::string worldBlob = world::Get().Serialize();
        const usize liveWithShard = LiveCount(s3);

        // The v2 snapshot: everything EXCEPT streamed-shard members.
        const auto notStreamed = [&s3](entt::entity e) {
            return !s3.Registry().all_of<StreamShard>(e);
        };
        const std::string snapV2 = scene::SaveSceneToString(s3, notStreamed);
        const std::string snapV1 = scene::SaveSceneToString(s3); // whole world (legacy shape)

        // --- v2 restore: snapshot -> AdoptWorld bind -> AdoptResidency(keys) ---
        {
            world::Get().Clear();
            world::Get().Deserialize(worldBlob);
            Scene s4;
            Streamer st4;
            scene::SceneData snap;
            expect(scene::ParseSceneString(snapV2, snap), "the v2 snapshot parses");
            scene::StagedAssets stg;
            scene::StageAssets(snap, dir, stg);
            scene::Instantiate(s4, renderer, snap, stg, scene::LoadMode::Replace);
            expect(ByGuid(s4, gGuard) == entt::null,
                   "the v2 snapshot really EXCLUDED the streamed shard's members");
            expect(ByGuid(s4, gDoor) != entt::null, "and really kept the resident set");
            expect(st4.BindLevel(s4, renderer, file, dir, defs, BindMode::AdoptWorld),
                   "bind AdoptWorld over the restored snapshot");
            expect(LiveCount(s4) < liveWithShard,
                   "AdoptWorld instantiated NOTHING of its own (no second resident set)");
            st4.AdoptResidency(s4, renderer, keys);
            expect(st4.IsResident(static_cast<u32>(st4.FindShard("Camp#0"))),
                   "the recorded shard is resident again");
            expect(LiveCount(s4) == liveWithShard,
                   "the restored world has exactly the saved entity count - nothing was "
                   "double-spawned");
            // The single most direct double-spawn pin: one entity per guid.
            u32 guards = 0;
            for (const entt::entity e : s4.Registry().view<const Guid>())
                if (s4.Registry().get<const Guid>(e).value == gGuard) ++guards;
            expect(guards == 1, "exactly ONE Guard exists (a double spawn would make two)");
            expect(s4.Registry().get<const Health>(ByGuid(s4, gGuard)).current == 7.0f,
                   "and its runtime state came back from the save's world:: blob");
            expect(world::Get().VisitCount(area) == 1,
                   "loading a save does NOT bump the visit count (the player never re-entered)");
            expect(AuditHandles(s4).dangling == 0, "a save restore leaves no dangling handle");
        }

        // --- v1 restore: the LEGACY whole-world snapshot ----------------------
        // A pre-shard save contains the shard's members as ordinary entities. Adopting
        // marks their shard resident so the streamer can manage them for the rest of the
        // session; without that it would believe nothing is resident and could never
        // despawn what the snapshot restored.
        {
            world::Get().Clear();
            world::Get().Deserialize(worldBlob);
            Scene s5;
            Streamer st5;
            scene::SceneData snap;
            expect(scene::ParseSceneString(snapV1, snap), "the v1-shape snapshot parses");
            scene::StagedAssets stg;
            scene::StageAssets(snap, dir, stg);
            scene::Instantiate(s5, renderer, snap, stg, scene::LoadMode::Replace);
            expect(ByGuid(s5, gGuard) != entt::null,
                   "a legacy snapshot carries the shard members itself");
            expect(st5.BindLevel(s5, renderer, file, dir, defs, BindMode::AdoptWorld),
                   "bind AdoptWorld over a legacy snapshot");
            const usize before = LiveCount(s5);
            st5.AdoptResidency(s5, renderer, {}); // a v1 save records no shard keys
            expect(LiveCount(s5) == before,
                   "adopting a legacy snapshot spawns NOTHING - it is already all there");
            const i32 c5 = st5.FindShard("Camp#0");
            expect(c5 >= 0 && st5.IsResident(static_cast<u32>(c5)),
                   "and the shard is now marked resident, so streaming works from here on");
            expect(st5.DespawnShard(s5, static_cast<u32>(c5)),
                   "which means the streamer can despawn what a legacy save restored");
            expect(ByGuid(s5, gGuard) == entt::null, "and it really goes away");
            expect(AuditHandles(s5).dangling == 0, "and that despawn leaves no dangling handle");
        }
    }

    fs::remove_all(dir, ec);
    world::Get().Clear();
    world::SetCurrentArea({});
    if (ok) HBE_INFO("shardstate: PASS");
    return ok;
}

} // namespace hbe::stream
