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

#include <glm/gtc/matrix_transform.hpp> // glm::translate - the association self-test's bake rows

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
    r.authoring = authoring_;
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
    authoring_ = false;
    silent_ = false;
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
    assoc_.graph.Clear();
    assoc_.seed.clear();
    assoc_.marked.clear();
    assoc_.visited.clear();
    assocActive_ = false;
    shardTag_.clear();
    alwaysLoadedTag_.clear();
    shardAssociated_.clear();
    // Session hysteresis for the association seed: it describes shards that no longer
    // exist, so carrying it across a rebind would hand shard 3's band to whatever shard
    // 3 becomes in the next level.
    shardSelfSeed_.clear();
    // A manual override is SESSION state that describes shards that no longer exist.
    // Carrying it across a rebind would apply shard 3's "forced out" to whatever shard
    // 3 happens to be in the next level.
    shardForce_.clear();
    heldShard_ = -1;
    if (scene) PublishResidency(*scene); // bound_ is false now: clears the summary
}

// RE-RESOLVE THE ASSOCIATION GRAPH AGAINST AN EDITED TAG LIST, mid-bind.
//
// BindLevel resolves it exactly once, which is right for a game (the project's tags
// cannot change while a level is running) and wrong for the editor, where the Tags
// panel edits the very list the bound streamer resolved from. Without this, an author
// with live zones on who associates Hill -> Vista and then stands on the hill sees
// NOTHING happen, with no log line and no warning, until they save (which re-binds).
//
// Deliberately touches ONLY the graph and its derived index tables. It does not read
// or write shards_, residency, forces or the seed history, so it is safe at any point
// in a bind; the next evaluation simply propagates over the new edges. forceEval_
// makes that evaluation the next frame rather than up to four frames away.
void Streamer::RefreshAssociations(const std::vector<TagDef>& defs) {
    if (!bound_) return;
    std::vector<std::string> unresolved;
    tags::BuildAssocGraph(defs, assoc_.graph, &unresolved);
    for (const std::string& u : unresolved)
        HBE_WARN("TagStream: association '{}' names a tag this project does not list; the "
                 "link is ignored (the authored name is kept).",
                 u);
    assocActive_ = false;
    for (const std::vector<u32>& e : assoc_.graph.edges)
        if (!e.empty()) {
            assocActive_ = true;
            break;
        }
    alwaysLoadedTag_.assign(defs.size(), 0u);
    for (usize t = 0; t < defs.size(); ++t)
        alwaysLoadedTag_[t] = defs[t].alwaysLoaded ? 1u : 0u;
    std::unordered_map<std::string, u32> tagIndex;
    tagIndex.reserve(defs.size());
    for (usize t = 0; t < defs.size(); ++t) tagIndex.emplace(defs[t].name, static_cast<u32>(t));
    shardTag_.assign(shards_.size(), kNoAssocTag);
    for (usize i = 0; i < shards_.size(); ++i) {
        const auto it = tagIndex.find(shards_[i]->desc.tag);
        if (it != tagIndex.end()) shardTag_[i] = it->second;
    }
    // The marks are DERIVED, so they are simply re-derived next evaluation. Clearing
    // them here would drop a hold for one frame and despawn a zone the author is
    // looking at, for no reason.
    shardAssociated_.resize(shards_.size(), 0u);
    shardSelfSeed_.resize(shards_.size(), 0u);
    forceEval_ = true;
}

// --- Manual overrides ---------------------------------------------------------

void Streamer::SetShardForce(u32 i, ShardForce f) {
    if (i >= shards_.size()) return;
    // Stay empty until something is actually forced: that is what makes the whole
    // feature free in the shipping runtime.
    if (shardForce_.empty()) {
        if (f == ShardForce::Auto) return;
        shardForce_.assign(shards_.size(), static_cast<u8>(ShardForce::Auto));
    }
    shardForce_[i] = static_cast<u8>(f);
    forceEval_ = true; // act on it now, not in up to four frames' time
}

ShardForce Streamer::ShardForceOf(u32 i) const {
    if (i >= shardForce_.size()) return ShardForce::Auto;
    return static_cast<ShardForce>(shardForce_[i]);
}

void Streamer::ClearShardForces() {
    shardForce_.clear();
    forceEval_ = true;
}

u32 Streamer::ForcedShardCount() const {
    u32 n = 0;
    for (const u8 f : shardForce_)
        if (static_cast<ShardForce>(f) != ShardForce::Auto) ++n;
    return n;
}

u32 Streamer::SpawnAllShards(Scene& scene, Renderer& renderer) {
    if (!bound_) return 0;
    // A staging job may be writing into a shard right now, and a shard already in
    // Ready would be finalized by a LATER Update - on top of the spawn below, which is
    // the double-spawn this drain-and-discard exists to prevent. Re-staging next time
    // is cheap (the process-wide caches make StageAssets skip anything resident).
    DrainInFlight();
    for (auto& sr : shards_) {
        const auto s = static_cast<salvage::RegionState>(sr->state.load(std::memory_order_acquire));
        if (s == salvage::RegionState::Ready || s == salvage::RegionState::Loading) {
            sr->staged = scene::StagedAssets{};
            sr->state.store(static_cast<int>(salvage::RegionState::Unloaded),
                            std::memory_order_release);
            sr->deferred = false;
        }
    }
    u32 missing = 0;
    for (usize i = 0; i < shards_.size(); ++i) {
        if (shards_[i]->resident) continue;
        // A Failed shard is TERMINAL and stays that way (see the header): spawning it
        // synchronously here would be the every-frame retry SALVAGE 3 removed. It is
        // counted instead, so the caller - the save path - still refuses.
        const auto s =
            static_cast<salvage::RegionState>(shards_[i]->state.load(std::memory_order_acquire));
        if (s == salvage::RegionState::Failed) {
            ++missing;
            continue;
        }
        if (!SpawnShard(scene, renderer, static_cast<u32>(i))) ++missing;
    }
    PublishResidency(scene);
    return missing;
}

void Streamer::ClearMembership(Scene& scene) const {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> holders;
    for (const entt::entity e : reg.view<const StreamShard>()) holders.push_back(e);
    for (const entt::entity e : holders)
        if (reg.valid(e)) reg.remove<StreamShard>(e);
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
    if (bound_ && (mode == BindMode::Fresh || mode == BindMode::MenuWorld))
        UnloadAll(scene); // capture inside is gated on authoring_/silent_
    Reset(&scene);
    // Set AFTER Reset, which clears it. Everything below that asks "is this the
    // editor's world?" reads this flag, including SpawnRows/DespawnShard's world::
    // calls, so it has to be true before the first spawn of the bind.
    authoring_ = (mode == BindMode::AuthorWorld);
    silent_ = (mode == BindMode::MenuWorld);
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

    // --- RULE 6: resolve the association graph, ONCE -------------------------
    // Names become indices here and never again: the per-evaluation pass is integer
    // adjacency with no string work. A name the project does not list cannot become
    // an index, so it is dropped and reported ONCE PER BIND - not per evaluation and
    // certainly not per frame, which is the difference between a diagnostic and a
    // log flood.
    {
        std::vector<std::string> unresolved;
        tags::BuildAssocGraph(defs, assoc_.graph, &unresolved);
        for (const std::string& u : unresolved) {
            HBE_WARN("TagStream: association '{}' names a tag this project does not list; "
                     "the link is ignored (the authored name is kept, so adding the tag "
                     "restores it).",
                     u);
        }
        assocActive_ = false;
        for (const std::vector<u32>& e : assoc_.graph.edges)
            if (!e.empty()) {
                assocActive_ = true;
                break;
            }
        // An alwaysLoaded tag has no ShardRuntime at all, so it could never enter a
        // shard-derived seed set - and silently doing nothing when the author wrote
        // an association on it is the astonishing outcome. It is PERMANENTLY seeded:
        // it always drives, which is the literal reading of the relation. The bake
        // warns loudly that its targets can therefore never unload.
        alwaysLoadedTag_.assign(defs.size(), 0u);
        for (usize t = 0; t < defs.size(); ++t)
            alwaysLoadedTag_[t] = defs[t].alwaysLoaded ? 1u : 0u;
        std::unordered_map<std::string, u32> tagIndex;
        tagIndex.reserve(defs.size());
        for (usize t = 0; t < defs.size(); ++t)
            tagIndex.emplace(defs[t].name, static_cast<u32>(t));
        shardTag_.assign(shards_.size(), kNoAssocTag);
        for (usize i = 0; i < shards_.size(); ++i) {
            const auto it = tagIndex.find(shards_[i]->desc.tag);
            if (it != tagIndex.end()) shardTag_[i] = it->second;
        }
        shardAssociated_.assign(shards_.size(), 0u);
        shardSelfSeed_.assign(shards_.size(), 0u);
    }

    for (usize r = 0; r < source_.entities.size(); ++r) {
        const scene::EntityData& d = source_.entities[r];
        // First row wins for a duplicate, matching Scene::FindByName's documented
        // arbitrary-match behaviour rather than inventing a second rule.
        if (d.guid != 0) guidToShard_.emplace(d.guid, rowToShard_[r]);
        if (!d.name.empty()) nameToRow_.emplace(d.name, static_cast<u32>(r));
    }

    bound_ = true;
    forceEval_ = true; // the first Update after a bind evaluates, cadence or not

    if (mode == BindMode::Fresh || mode == BindMode::MenuWorld) {
        // THE LEVEL owns the environment, not a shard: this clears the previous world
        // and applies ambient/exposure/shadowDistance/post/giSource exactly as a full
        // LoadMode::Replace would, which a slice may never do. Calling BindLevel twice
        // re-binds rather than stacking a second world.
        scene::BindWorld(scene, renderer, source_);
        // MenuWorld is the one Fresh-shaped bind that does NOT enter the area: a menu
        // is not a visit (AreaVisitCount.FirstVisit must survive for the real entry),
        // and with no current area the silent_ gates below have nothing to write.
        if (mode == BindMode::Fresh)
            world::EnterArea(areaId_); // one visit per area entry, before any RestoreGroup
        SpawnRows(scene, renderer, residentRows_, world::kResidentKey, -1, nullptr);
    } else if (mode == BindMode::AuthorWorld) {
        // THE EDITOR'S BIND, AND THE FOUR THINGS IT DOES NOT DO.
        //
        //   * NOT scene::BindWorld. Its first statement is DestroyWorld, which wipes
        //     every non-Persistent entity and BumpWorldTokens - i.e. it destroys the
        //     scene being edited and then makes Ctrl+S refuse it forever. This one
        //     call is the entire reason the editor could not stream, and skipping it
        //     is the whole trick.
        //   * NOT ApplyEnvironment. The editor's own load already applied it; doing
        //     it again would stamp the FILE's environment over any unsaved lighting.
        //   * NOT world::EnterArea / SetCurrentArea. A visit bump is player progress,
        //     and authoring must never write it.
        //   * NOT SpawnRows(residentRows_). Those rows are already standing. Spawning
        //     them again would not even produce duplicates of the same objects - it
        //     would produce duplicates with FRESH GUIDS, because guid::Claim refuses
        //     to adopt a guid that is already taken.
        //
        // Nothing at all happens here, deliberately. AdoptResidency is what makes the
        // streamer own what is already in the registry.
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
             mode == BindMode::AdoptWorld  ? " (adopting a restored world)"
             : mode == BindMode::AuthorWorld ? " (AUTHORING the editor's world in place)"
             : mode == BindMode::MenuWorld   ? " (MENU backdrop: no visits, no captures)"
                                             : "");
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
    //
    // NOT WHILE AUTHORING. world:: holds PLAYER PROGRESS - opened doors, killed NPCs,
    // taken pickups - and RestoreGroup will DESTROY any member that was absent when
    // the group was captured. In the editor nothing reverts those writes, so a zone
    // toggled off and on again would come back missing whatever the last play session
    // had removed. Authoring has no deltas worth preserving; it wants the authored
    // rows, every time.
    if (!authoring_ && !silent_) world::RestoreGroup(scene, areaId_, shardKey, created);
    if (createdOut) *createdOut = std::move(created);
}

// Respawns an authoring shard from the string DespawnShard captured off the live
// registry, rather than from the level file. False when there is no snapshot or it
// does not parse - the caller then falls back to the file, which is the old behaviour
// and still better than not spawning at all.
bool Streamer::SpawnAuthorSnapshot(Scene& scene, Renderer& renderer, u32 i) {
    ShardRuntime& sr = *shards_[i];
    if (sr.authorSnapshot.empty()) return false;
    scene::SceneData data;
    if (!scene::ParseSceneString(sr.authorSnapshot, data)) {
        HBE_WARN("TagStream: the authoring snapshot for '{}' did not parse; respawning from "
                 "the file instead (edits made inside that zone are lost).",
                 ShardKey(i));
        sr.authorSnapshot.clear();
        return false;
    }
    scene::StagedAssets staged;
    scene::StageAssets(data, assetsDir_, staged);
    std::vector<entt::entity> created;
    // ADDITIVE, so no environment is applied and no WorldToken is bumped - the same
    // shape SpawnRows uses. The guids were freed by the destroy, so they come back
    // unchanged and every membership/adoption table still answers correctly.
    scene::Instantiate(scene, renderer, data, staged, scene::LoadMode::Additive, &created);
    entt::registry& reg = scene.Registry();
    for (const entt::entity e : created)
        if (reg.valid(e)) reg.emplace_or_replace<StreamShard>(e, StreamShard{i});
    sr.lastCreated = std::move(created);
    return true;
}

bool Streamer::SpawnShard(Scene& scene, Renderer& renderer, u32 i) {
    if (!bound_ || i >= shards_.size()) return false;
    ShardRuntime& sr = *shards_[i];
    if (sr.resident) return false;
    if (authoring_ && SpawnAuthorSnapshot(scene, renderer, i)) {
        ++stats_.syncStages;
        sr.resident = true;
        sr.state.store(static_cast<int>(salvage::RegionState::Loaded), std::memory_order_release);
        ++stats_.spawns;
        stats_.residentPeak = glm::max(stats_.residentPeak, ResidentShardCount());
        PublishResidency(scene);
        HBE_TRACE("TagStream: respawned authoring shard '{}' from its editor snapshot ({} "
                  "entities).",
                  ShardKey(i), sr.lastCreated.size());
        return true;
    }
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
    // re-armed spawner progress is what gets recorded. (Authoring skips it with the
    // capture below - there is no encounter in progress to re-arm.)
    if (!authoring_ && !silent_) MarkStreamedOutPopulation(scene, members);
    // CAPTURE FIRST, ALWAYS. Everything below this line is irreversible, and a despawn
    // that destroys before it records is exactly the regression that made automatic
    // streaming unshippable without this step.
    // See SpawnRows: an AUTHORING bind writes nothing into world::. Capturing here
    // would record the AUTHORED scene as a set of player-progress deltas against
    // itself, which the next real play session would then replay.
    if (!authoring_ && !silent_) world::CaptureGroup(scene, areaId_, ShardKey(i), members);
    // AUTHORING'S EQUIVALENT OF THAT CAPTURE, and for the same reason: everything below
    // this line is irreversible. The editor's respawn source becomes what is standing
    // now, not what the file said at bind - so a move, a component edit or a brand-new
    // child inside this zone survives the round trip instead of being silently reverted
    // (or, for something the file has never heard of, destroyed for good). See
    // ShardRuntime::authorSnapshot.
    if (authoring_) {
        std::unordered_set<u32> keep;
        keep.reserve(members.size() * 2);
        for (const entt::entity e : members) keep.insert(static_cast<u32>(e));
        sr.authorSnapshot = scene::SaveSceneToString(scene, [&keep](entt::entity e) {
            return keep.count(static_cast<u32>(e)) > 0;
        });
    }
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
    // always-loaded half of it. (Authoring writes no world:: state - see SpawnRows.)
    if (!authoring_ && !silent_) world::CaptureArea(scene, areaId_);
    for (usize i = 0; i < shards_.size(); ++i)
        if (shards_[i]->resident) DespawnShard(scene, static_cast<u32>(i));
}

void Streamer::CaptureAllLoaded(const Scene& scene) {
    if (!bound_ || authoring_ || silent_) return; // authoring/menu write no world:: state
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

// Worker-thread staging for an AUTHORING respawn - mirrors StageJob but sources the
// captured snapshot (the author's edits) instead of the level file, so the editor's
// respawn no longer parses + generates mips + decodes textures synchronously on the main
// thread (the ~1.5s editor load spike). Finalize then only has to instantiate + upload.
void Streamer::SnapshotStageJob(void* arg) {
    ShardRuntime* sr = static_cast<ShardRuntime*>(arg);
    Streamer* self = sr->owner;
    salvage::RegionState next = salvage::RegionState::Ready;
    try {
        sr->staged = scene::StagedAssets{}; // stage into a cleared buffer (SALVAGE 3)
        auto data = std::make_unique<scene::SceneData>();
        if (scene::ParseSceneString(sr->authorSnapshot, *data)) {
            scene::StageAssets(*data, self->assetsDir_, sr->staged);
            sr->snapshotData = std::move(data); // Finalize instantiates from this
        } else {
            // Corrupt snapshot: drop it so Finalize falls back to the file slice - the same
            // fallback SpawnAuthorSnapshot always had (edits inside that zone are lost).
            HBE_WARN("TagStream: authoring snapshot for '{}' did not parse; respawning from "
                     "the file (edits inside that zone are lost).",
                     sr->desc.tag);
            sr->authorSnapshot.clear();
            sr->snapshotData.reset();
        }
    } catch (const std::exception& e) {
        HBE_ERROR("TagStream: snapshot staging '{}' threw: {}", sr->desc.tag, e.what());
        sr->staged = scene::StagedAssets{};
        sr->snapshotData.reset();
        next = salvage::RegionState::Failed;
    } catch (...) {
        sr->staged = scene::StagedAssets{};
        sr->snapshotData.reset();
        next = salvage::RegionState::Failed;
    }
    sr->state.store(static_cast<int>(next), std::memory_order_release);
    self->inFlight_.fetch_sub(1, std::memory_order_release);
}

void Streamer::Finalize(Scene& scene, Renderer& renderer, u32 i) {
    ShardRuntime& sr = *shards_[i];
    std::vector<entt::entity> created;
    if (sr.snapshotData) {
        // AUTHORING respawn: instantiate the whole captured snapshot (staged off the main
        // thread by SnapshotStageJob), not a slice of the level file. Frees it after use.
        scene::Instantiate(scene, renderer, *sr.snapshotData, sr.staged,
                           scene::LoadMode::Additive, &created);
        sr.snapshotData.reset();
    } else {
        scene::Instantiate(scene, renderer, source_, sr.staged, scene::LoadMode::Additive,
                           &created, /*sceneTag*/ {}, SlicePtr(sr.rows),
                           static_cast<u32>(sr.rows.size()));
    }
    entt::registry& reg = scene.Registry();
    for (const entt::entity e : created)
        if (reg.valid(e)) reg.emplace_or_replace<StreamShard>(e, StreamShard{i});
    // Replay the captured deltas the same frame the entities appear, so nothing ever
    // observes the authored state of a shard the player already changed.
    //
    // NOT WHILE AUTHORING - the same guard SpawnRows, DespawnShard, UnloadAll and
    // CaptureAllLoaded all carry, and this was the one path missing it. It is the path
    // the EDITOR actually takes: Engine::Initialize starts the job system, so every
    // automatic respawn goes StageJob -> Ready -> here, never through SpawnRows.
    // RestoreGroup DESTROYS members that were absent when the group was captured and
    // overwrites component state on the rest - on the world the author is editing,
    // which the next Ctrl+S then writes.
    if (!authoring_ && !silent_) world::RestoreGroup(scene, areaId_, ShardKey(i), created);
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
    // RULE 6: WHICH SHARDS ARE HELD BY AN ASSOCIATED TAG. Two steps, both O(tags +
    // edges + shards) and both inside the timed region below, so the added cost is
    // reported honestly in stats_.lastEvalMs rather than hidden:
    //
    //   1. SEED, FROM DISTANCE ONLY. A tag is seeded when one of its shards is in
    //      range on its OWN terms - inside its load radius, or still inside its
    //      unload radius having been inside the load radius. Seeding from
    //      association-derived residency instead would let a mutual pair hold each
    //      other resident forever with the player 10 km away.
    //
    //      THE SEED CARRIES ITS OWN HYSTERESIS (shardSelfSeed_) rather than borrowing
    //      the shard's `resident` flag. `resident` only becomes true at the main-thread
    //      finalize - and NEVER for a Failed shard, which is terminal - so borrowing it
    //      degenerated the band to the bare `d <= loadRadius` test for every driver
    //      that had not finished staging or could not stage at all. A focus oscillating
    //      on such a driver's load boundary then toggled the seed, hence the driven
    //      tag's `associated`, hence a spawn/despawn of an entire distant city every
    //      few frames - exactly the thrash RULE 2 exists to prevent, reintroduced
    //      through the association path. shardSelfSeed_ is still purely
    //      DISTANCE-derived (it clears the moment d exceeds the unload radius), so the
    //      cycle-collapse argument in StreamPolicy.h is untouched.
    //   2. PROPAGATE, capped and cycle-safe (stream::AssocPass - the same call the
    //      editor's prediction makes).
    //
    // Skipped entirely when the project authored no association at all, so a project
    // that does not use the feature pays one branch. Skipped with NO FOCUS too:
    // rule 3 says an empty focus list changes nothing, and re-deriving the marks from
    // "everything is infinitely far away" would drop them.
    //
    // A MANUAL OVERRIDE COMPOSES THROUGH THIS SAME SEED SET rather than being a
    // second notion of residency: "force resident" SEEDS the tag (so forcing the hill
    // brings in the vista, which is what an author forcing a zone in expects), and
    // "force unloaded" is excluded from seeding (so it stops driving) and has its
    // mark cleared afterwards (so it stops being driven). One mechanism, not two.
    if (assocActive_ && !foci.empty()) {
        assoc_.BeginSeed();
        for (usize t = 0; t < alwaysLoadedTag_.size() && t < assoc_.seed.size(); ++t)
            if (alwaysLoadedTag_[t]) assoc_.seed[t] = 1u;
        // EVERY shard is measured, not just the ones that would flip a not-yet-set
        // seed: shardSelfSeed_ is per-SHARD state (it carries that shard's own
        // hysteresis) and it is also the "is this shard here on its own terms?"
        // answer the author-facing readout needs. O(shards x foci), the same order
        // Evaluate below already is.
        shardSelfSeed_.resize(shards_.size(), 0u);
        for (usize i = 0; i < shards_.size(); ++i) {
            const ShardRuntime& sr = *shards_[i];
            const f32 d = NearestFocusDistance(static_cast<u32>(i), foci);
            bool self = d <= sr.loadRadius ||
                        (shardSelfSeed_[i] != 0u && d <= sr.unloadRadius);
            switch (ShardForceOf(static_cast<u32>(i))) {
            case ShardForce::Unloaded: self = false; break; // a zone forced out drives nothing
            case ShardForce::Resident: self = true; break;  // ...and one forced in DOES drive
            case ShardForce::Auto: break;
            }
            shardSelfSeed_[i] = self ? 1u : 0u;
            const u32 t = shardTag_[i];
            if (self && t != kNoAssocTag && t < assoc_.seed.size()) assoc_.seed[t] = 1u;
        }
        assoc_.Run();
        // MARK EVERY SHARD OF A DRIVEN TAG. The seed is OR'd over a tag's shards but a
        // hold has to apply to each of them individually: with a tag that autoShards
        // into three cells, one cell coming into its own range must not release the
        // other two while the driver is still driving. That is what the seed/visited
        // split in AssocPass::Run is for - `marked` is now set on a tag even when the
        // tag is itself seeded.
        shardAssociated_.assign(shards_.size(), 0u);
        for (usize i = 0; i < shards_.size(); ++i) {
            const u32 t = shardTag_[i];
            if (t != kNoAssocTag && t < assoc_.marked.size() && assoc_.marked[t])
                shardAssociated_[i] = 1u;
        }
    } else if (!assocActive_ && !shardForce_.empty()) {
        // No association graph at all, but the overrides still have to be expressible:
        // the flag is the vehicle for "resident for a reason that is not distance", so
        // a forced-in shard uses it here exactly as an associated one would.
        //
        // Deliberately NOT the `assocActive_ && foci.empty()` case. Rule 3 says an
        // empty focus list changes nothing, so the marks from the last evaluation must
        // survive it; clearing them here would drop a driven shard the moment the
        // player and the camera both went away.
        shardAssociated_.assign(shards_.size(), 0u);
    }
    // Apply the overrides to the derived flag, AFTER propagation: Unloaded beats being
    // driven, Resident is held the same way an association holds a shard.
    if (!shardForce_.empty()) {
        shardAssociated_.resize(shards_.size(), 0u);
        for (usize i = 0; i < shards_.size(); ++i) {
            switch (ShardForceOf(static_cast<u32>(i))) {
            case ShardForce::Resident: shardAssociated_[i] = 1u; break;
            case ShardForce::Unloaded: shardAssociated_[i] = 0u; break;
            case ShardForce::Auto: break;
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
        // heldShard_ is the editor holding the SELECTION's shard. It is OR'd into
        // `pinned` because that is already exactly "never unload this", and it is
        // deliberately not a third bool: the author never sees it and it must not read
        // as a manual override in the panel.
        p.pinned = pinnedByMember_[i] != 0u || static_cast<i32>(i) == heldShard_;
        p.failed = (s == salvage::RegionState::Failed);
        // Kept SEPARATE from `pinned` on purpose: the two are different answers to
        // "why is this shard here?", and the readout has to be able to say which.
        p.associated = i < shardAssociated_.size() && shardAssociated_[i] != 0u;
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
    // MANUAL "FORCE UNLOADED" IS APPLIED HERE, NOT IN THE POLICY. Rule 2 refuses to
    // unload a shard a focus is inside, and that refusal is load-bearing - it is what
    // stops boundary thrash - so a seventh policy rule that punched through it would
    // weaken the thing every other caller depends on. An override is not a streaming
    // rule; it is the author overruling the streamer for this session, and it belongs
    // where the streamer acts. The list is EMPTY in the shipping runtime.
    if (!shardForce_.empty()) {
        // Never START a load for a zone the author has switched off - including the
        // loads `enabled == false` orders, which is how "live streaming off" and "this
        // one zone off" coexist.
        policyOut_.load.erase(std::remove_if(policyOut_.load.begin(), policyOut_.load.end(),
                                             [this](u32 i) {
                                                 return ShardForceOf(i) == ShardForce::Unloaded;
                                             }),
                              policyOut_.load.end());
        // And despawn it however close the focus is. Appended rather than replacing the
        // policy's list, and deduplicated, so a shard the policy ALSO wanted gone is
        // not despawned twice (DespawnShard is a no-op on a non-resident shard, but
        // relying on that would make the stats lie).
        for (usize i = 0; i < shards_.size(); ++i) {
            if (ShardForceOf(static_cast<u32>(i)) != ShardForce::Unloaded) continue;
            if (!shards_[i]->resident) continue;
            const u32 idx = static_cast<u32>(i);
            if (std::find(policyOut_.unload.begin(), policyOut_.unload.end(), idx) ==
                policyOut_.unload.end())
                policyOut_.unload.push_back(idx);
        }
    }
    for (const u32 i : policyOut_.load) {
        ShardRuntime& sr = *shards_[i];
        // AN AUTHORING SHARD WITH AN EDITOR SNAPSHOT COMES BACK FROM THE SNAPSHOT, and
        // therefore synchronously: the staging job reads the level FILE, which is
        // exactly the source that would revert the author's edits. This is the editor,
        // one zone at a time, so a synchronous spawn here is the same cost
        // SpawnAllShards already pays on every save.
        if (authoring_ && !sr.authorSnapshot.empty()) {
            // Stage the SNAPSHOT (the author's edits, not the file) off the main thread
            // when the job system is up, so the editor's respawn does its parse + mip-gen +
            // texture decode on a worker and only the cheap instantiate/upload lands on the
            // main thread. Without this the editor took a ~1.5s synchronous load hitch on
            // every zone respawn. Falls back to the fully-synchronous path when there is no
            // job system (a headless tool / self-test).
            if (jobs::IsInitialized()) {
                sr.state.store(static_cast<int>(salvage::RegionState::Loading),
                               std::memory_order_release);
                ++stats_.asyncStages;
                inFlight_.fetch_add(1, std::memory_order_acquire);
                jobs::RunDetached(&Streamer::SnapshotStageJob, &sr, jobs::Priority::Normal);
            } else {
                const u32 idx = i;
                timeStructural([&] { SpawnShard(scene, renderer, idx); });
            }
            continue;
        }
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
    // Reclaim the GPU memory of any cached mesh/texture the just-despawned entities were
    // the LAST users of (a mark-sweep over all resident entities). Once per Update after
    // despawns - not per-despawn. RUNTIME ONLY: in the editor, cached handles are also
    // referenced by non-entity holders (asset previews, thumbnails, the material editor)
    // that the entity-only mark can't see, so sweeping there could free something still in
    // use; the editor also keeps content resident for editing. This is the fix for
    // 'streaming despawn reclaims ZERO VRAM' - the leak only matters in a long play session.
    // Only sweep on a backend that truly frees VRAM in DestroyMesh/DestroyTexture. On one
    // that still no-ops them, dropping resources from the shared cache would force a re-upload
    // on the next respawn (and leak the old handle), which is strictly worse than never
    // reclaiming - so leave the cache intact there until that backend implements reclaim.
    if (!authoring_ && !policyOut_.unload.empty() && renderer.SupportsResourceReclaim())
        scene::TrimUnreferencedGpu(scene, renderer);
}

bool Streamer::IsSettled(const std::vector<glm::vec3>& foci) const {
    if (!bound_) return true;
    for (usize i = 0; i < shards_.size(); ++i) {
        const auto s =
            static_cast<salvage::RegionState>(shards_[i]->state.load(std::memory_order_acquire));
        // "In range" is the same distance-to-BOX test the policy uses; with no focus at
        // all nothing is in range, which is right - there is nothing to wait for.
        //
        // RULE 6 COUNTS AS IN RANGE. A shard held by an association is out of range BY
        // DEFINITION, so without this clause an Unloaded associated shard reads as
        // settled and the loading screen drops before it appears - which is exactly
        // the pop the screen exists to hide, and the difference between the distant
        // city fading in behind the curtain and popping in front of the player.
        //
        // AND A MANUALLY UNLOADED ZONE IS SETTLED, whatever the distance says. The
        // author switched it off; waiting for it to appear would hang forever.
        if (ShardForceOf(static_cast<u32>(i)) == ShardForce::Unloaded) {
            if (!salvage::RegionSettled(s, false)) return false;
            continue;
        }
        const bool inRange =
            (!foci.empty() &&
             NearestFocusDistance(static_cast<u32>(i), foci) <= shards_[i]->loadRadius) ||
            (i < shardAssociated_.size() && shardAssociated_[i] != 0u);
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

    // --- MenuWorld: a menu backdrop leaves ZERO trace in persistence ----------
    // The 3D main menu binds the startup scene behind the menu. The contract, and
    // the save-corruption risk it prevents: a menu is NOT a visit (the
    // AreaVisitCount schematic node's FirstVisit must still fire on the player's
    // REAL first entry), and menu-time spawn/despawn must never capture - a capture
    // would write the AUTHORED state over whatever a real save holds.
    {
        world::Get().Clear();
        world::SetCurrentArea({});
        const std::string blankBlob = world::Get().Serialize();

        Scene sm;
        Streamer stm;
        expect(stm.BindLevel(sm, renderer, file, dir, defs, BindMode::MenuWorld),
               "MenuWorld binds the fixture level");
        expect(LiveCount(sm) > 0, "the resident slice really spawned (a backdrop is visible)");
        expect(world::CurrentArea().empty(),
               "MenuWorld did NOT enter the area (a menu is not a visit)");

        // Stream a shard in and out again, as the menu camera would by distance.
        const i32 cm = stm.FindShard("Camp#0");
        expect(cm >= 0, "the fixture shard exists");
        if (cm >= 0) {
            expect(stm.SpawnShard(sm, renderer, static_cast<u32>(cm)),
                   "a shard spawns behind the menu");
            expect(stm.DespawnShard(sm, static_cast<u32>(cm)),
                   "and despawns when the menu camera moves off");
        }
        expect(world::Get().Serialize() == blankBlob,
               "a full menu spawn/despawn cycle wrote NOTHING into world:: - zero trace");

        // Teardown mirrors FlowPlay: Reset (no capture), then the REAL first entry.
        stm.Reset(&sm);
        expect(world::Get().Serialize() == blankBlob,
               "tearing the menu down for Play captured nothing either");
        Scene sp;
        Streamer stp;
        expect(stp.BindLevel(sp, renderer, file, dir, defs, BindMode::Fresh),
               "the real Play bind still works after a menu bind");
        expect(!world::CurrentArea().empty(), "Fresh entered the area");
        expect(world::Get().VisitCount(world::CurrentArea()) == 1,
               "and the PLAYER'S entry is visit #1 - the menu consumed no FirstVisit");
        stp.Reset(&sp);
    }

    fs::remove_all(dir, ec);
    world::Get().Clear();
    world::SetCurrentArea({});
    if (ok) HBE_INFO("shardstate: PASS");
    return ok;
}

// --- --test-assoctags: RULE 6, associated tags --------------------------------

bool AssocSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("assoctags: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_assoctags";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "Valley.hbscene";

    // THE AUTHORED SCENARIO, BY NAME. Three tags, THREE SEPARATE PIECES OF CONTENT,
    // each with its own objects, its own baked bounds and its own radii:
    //
    //   City          the real city, x in [0, 100]      - load 100
    //   City_LowPoly  a DIFFERENT asset the author made for distant viewing,
    //                 x in [200, 300]                   - load 60
    //   Hill          the vantage point, x around 2000  - load 300
    //
    // Hill ASSOCIATES City_LowPoly, one-way. Nothing else relates them: City and
    // City_LowPoly do not know about each other, are not a LOD pair, and are not
    // mutually exclusive. Whether both are resident at once is the author's business
    // and this test asserts NOTHING about it.
    const auto makeDefs = [] {
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        TagDef city;
        city.name = "City";
        city.loadRadius = 100.0f;
        city.unloadRadius = 130.0f;
        TagDef low;
        low.name = "City_LowPoly";
        low.loadRadius = 60.0f;
        low.unloadRadius = 80.0f;
        TagDef hill;
        hill.name = "Hill";
        hill.loadRadius = 300.0f;
        hill.unloadRadius = 400.0f;
        std::vector<TagDef> d = {untagged, city, low, hill};
        return d;
    };
    std::vector<TagDef> defs = makeDefs();
    defs[3].associates.push_back("City_LowPoly"); // Hill pulls in the low-poly city
    tags::Normalize(defs);
    tags::SeedFromProject(defs);
    expect(defs.size() == 4 && defs[3].name == "Hill" && defs[3].associates.size() == 1,
           "the authored tag list survives Normalize with its association intact");

    const TagId tCity = tags::Intern("City"), tLow = tags::Intern("City_LowPoly"),
                tHill = tags::Intern("Hill");
    u64 gLowFirst = 0, gHillFirst = 0;
    {
        Scene s;
        auto& reg = s.Registry();
        const auto make = [&](const char* n, const glm::vec3& p, TagId tag) {
            const entt::entity e = s.CreateEntity(n);
            Transform t;
            t.position = p;
            reg.emplace<Transform>(e, t);
            reg.emplace<AABB>(e, AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});
            tags::Assign(reg, e, tag);
            return e;
        };
        make("Ground", {0.0f, -1.0f, 0.0f}, kTagUntagged); // untagged: always resident
        for (int i = 0; i < 5; ++i)
            make("CityBlock", {25.0f * static_cast<f32>(i), 0.0f, 0.0f}, tCity);
        for (int i = 0; i < 5; ++i) {
            const entt::entity e =
                make("CityBlock_LowPoly", {200.0f + 25.0f * static_cast<f32>(i), 0.0f, 0.0f}, tLow);
            if (gLowFirst == 0) gLowFirst = reg.get<Guid>(e).value;
        }
        for (int i = 0; i < 3; ++i) {
            const entt::entity e =
                make("HillRock", {1990.0f + 10.0f * static_cast<f32>(i), 0.0f, 0.0f}, tHill);
            if (gHillFirst == 0) gHillFirst = reg.get<Guid>(e).value;
        }
        const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
        expect(rep.errors == 0, "the three-tag valley bakes without errors");
        expect(rep.warnings == 0,
               "and WITHOUT warnings: an association whose driver and target are both in "
               "this level is the correct case and must be silent");
        expect(scene::SaveScene(s, file, {}, SceneKind::Full, &rep.shards),
               "save the valley with its baked shard header");
    }

    Renderer renderer; // device-less: UploadMesh returns an invalid handle, no GPU
    const glm::vec3 kOnTheHill(2000.0f, 0.0f, 0.0f);
    const glm::vec3 kInTheLowPolyCity(250.0f, 0.0f, 0.0f);
    const glm::vec3 kFarAway(100000.0f, 0.0f, 0.0f);
    const auto pump = [&renderer](Streamer& st, Scene& s, const glm::vec3& p, int frames) {
        std::vector<glm::vec3> foci{p};
        for (int i = 0; i < frames; ++i) st.Update(s, renderer, foci);
    };

    f64 worstEvalMs = 0.0, meanEvalMs = 0.0;

    // --- 1. The scenario itself ----------------------------------------------
    {
        world::Get().Clear();
        world::SetCurrentArea({});
        Scene s;
        Streamer st;
        expect(st.BindLevel(s, renderer, file, dir, defs), "bind the valley");
        if (!ok) {
            fs::remove_all(dir, ec);
            return false;
        }
        const i32 iCity = st.FindShard("City#0"), iLow = st.FindShard("City_LowPoly#0"),
                  iHill = st.FindShard("Hill#0");
        expect(iCity >= 0 && iLow >= 0 && iHill >= 0,
               "all three tags baked to their OWN shard - they are separate content");
        if (!ok) {
            fs::remove_all(dir, ec);
            return false;
        }
        const u32 uCity = static_cast<u32>(iCity), uLow = static_cast<u32>(iLow),
                  uHill = static_cast<u32>(iHill);
        const usize base = LiveCount(s);

        pump(st, s, kFarAway, 8);
        expect(st.ResidentShardCount() == 0, "100 km away nothing is resident");

        // THE FEATURE. Stand on the hill: the hill loads by distance, and the low-poly
        // city - 1.7 km away, far outside its own 60 m load radius - comes with it.
        pump(st, s, kOnTheHill, 12);
        expect(st.IsResident(uHill), "standing on the hill loads the Hill tag by distance");
        expect(st.IsResident(uLow),
               "and City_LowPoly becomes resident TOO, 1.7 km outside its own load radius, "
               "purely because Hill associates it");
        expect(st.IsAssociated(uLow),
               "and the streamer can say WHY: it is held by an association, not by distance");
        expect(!st.IsAssociated(uHill), "the driver itself is not 'associated' - it is in range");
        expect(!st.IsResident(uCity),
               "the REAL city is 1.9 km away and nothing associates it, so it stays out");
        expect(ByGuid(s, gLowFirst) != entt::null,
               "the low-poly city's entities really spawned - this is a spawn, not a flag");
        expect(ByGuid(s, gHillFirst) != entt::null, "and so did the hill's");

        // LEAVING THE HILL RELEASES IT - unless it is in range on its own. Walk from
        // the hill straight into the low-poly city: the hill unloads (association
        // gone) but the low-poly city is now inside its OWN radius and must NEVER be
        // despawned in between. Exactly one despawn happens: the hill's.
        const u32 despawnsBefore = st.Stats().despawns;
        pump(st, s, kInTheLowPolyCity, 12);
        expect(!st.IsResident(uHill), "walking off the hill unloads the Hill tag");
        expect(st.IsResident(uLow), "the low-poly city stays: it is now in range on its OWN");
        expect(!st.IsAssociated(uLow), "and the reason has changed from association to distance");
        expect(st.Stats().despawns - despawnsBefore == 1,
               "a shard resident for TWO reasons survives losing one: exactly ONE despawn "
               "(the hill) happened, so City_LowPoly was never dropped and respawned");

        // Now lose the second reason too.
        pump(st, s, kFarAway, 24);
        expect(st.ResidentShardCount() == 0,
               "with neither the association nor the distance, everything unloads");
        expect(LiveCount(s) == base, "and the entity count returns exactly to the baseline");

        // And it is reproducible, not a one-shot.
        pump(st, s, kOnTheHill, 12);
        expect(st.IsResident(uLow) && st.IsAssociated(uLow),
               "returning to the hill pulls the low-poly city back in");
        pump(st, s, kFarAway, 24);
        expect(st.ResidentShardCount() == 0 && LiveCount(s) == base,
               "and leaving releases it again - the association leaks nothing");
        worstEvalMs = glm::max(worstEvalMs, st.Stats().maxEvalMs);
        // The MEAN is the number that describes the steady state. The maximum above
        // is the FIRST evaluation after a bind, which pays for the one-off `assign`
        // of every scratch vector in the pass and is not what a running frame costs.
        if (st.Stats().evaluations > 0)
            meanEvalMs = st.Stats().totalEvalMs / static_cast<f64>(st.Stats().evaluations);
    }

    // --- 2. A CYCLE terminates ------------------------------------------------
    // Mutual association is an authoring mistake, not a crash and not a leak. The
    // propagation is seeded from DISTANCE only, so once neither member is within its
    // own unload radius the whole cycle collapses. Seeded from association-derived
    // residency it would hold itself resident forever with the player 100 km away.
    {
        std::vector<TagDef> cyc = makeDefs();
        cyc[3].associates.push_back("City_LowPoly");
        cyc[2].associates.push_back("Hill"); // ... and back again
        tags::Normalize(cyc);
        world::Get().Clear();
        world::SetCurrentArea({});
        Scene s;
        Streamer st;
        expect(st.BindLevel(s, renderer, file, dir, cyc), "bind with a MUTUAL association");
        const i32 iLow = st.FindShard("City_LowPoly#0"), iHill = st.FindShard("Hill#0");
        if (iLow >= 0 && iHill >= 0) {
            const u32 uLow = static_cast<u32>(iLow), uHill = static_cast<u32>(iHill);
            pump(st, s, kFarAway, 12);
            expect(st.ResidentShardCount() == 0,
                   "a cycle with NO distance seed marks nothing - it does not bootstrap "
                   "itself into residency");
            pump(st, s, kOnTheHill, 12);
            expect(st.IsResident(uHill) && st.IsResident(uLow),
                   "standing on the hill still loads both ends of the cycle");
            pump(st, s, kFarAway, 30);
            expect(st.ResidentShardCount() == 0,
                   "and walking away UNLOADS BOTH: the cycle terminates instead of pinning "
                   "the world resident forever");
        }
        worstEvalMs = glm::max(worstEvalMs, st.Stats().maxEvalMs);
    }

    // --- 3. A MISSING target warns and does not crash -------------------------
    {
        std::vector<TagDef> ghost = makeDefs();
        ghost[3].associates.push_back("City_LowPoly_ThatWasRenamed"); // no such tag
        tags::Normalize(ghost);
        expect(ghost[3].associates.size() == 1,
               "Normalize KEEPS a dangling association - validation is a report, not a "
               "deletion of authored intent");
        world::Get().Clear();
        world::SetCurrentArea({});
        Scene s;
        Streamer st;
        expect(st.BindLevel(s, renderer, file, dir, ghost),
               "a binding whose association names a tag that does not exist still BINDS");
        const i32 iLow = st.FindShard("City_LowPoly#0"), iHill = st.FindShard("Hill#0");
        if (iLow >= 0 && iHill >= 0) {
            pump(st, s, kOnTheHill, 12);
            expect(st.IsResident(static_cast<u32>(iHill)),
                   "the driver streams normally - a broken link costs the LINK, not the tag");
            expect(!st.IsResident(static_cast<u32>(iLow)),
                   "and the unresolvable link pulls nothing in");
            pump(st, s, kFarAway, 24);
            expect(st.ResidentShardCount() == 0, "and unloading still works");
        }
    }

    // --- 4. Cost -------------------------------------------------------------
    // The pass is O(tags + edges) on an EVALUATION frame only, so it inherits the
    // 4-frame / 2-metre cadence. Measured against the streamer's own eval timer,
    // which is the number the engine's Perf line prints.
    {
        expect(meanEvalMs < 0.01,
               "the MEAN evaluation, association pass included, stays inside the "
               "0.002-0.006 ms envelope streaming already had");
        expect(worstEvalMs < 0.25,
               "and even the first-evaluation-after-bind worst case (which pays for the "
               "one-off scratch allocation) stays far inside the streaming budget");
        // And at a realistic project scale, isolated: 32 tags, 40 edges, 20k runs.
        AssocPass ap;
        constexpr u32 kTags = 32;
        ap.graph.edges.assign(kTags, {});
        for (u32 t = 0; t < 40; ++t) ap.graph.edges[t % kTags].push_back((t * 7 + 3) % kTags);
        const auto t0 = std::chrono::high_resolution_clock::now();
        constexpr u32 kRuns = 20000;
        u32 sink = 0;
        for (u32 r = 0; r < kRuns; ++r) {
            ap.BeginSeed();
            ap.seed[r % kTags] = 1u;
            ap.Run();
            for (const u8 m : ap.marked) sink += m;
        }
        const f64 perRunMs =
            std::chrono::duration<f64, std::milli>(std::chrono::high_resolution_clock::now() - t0)
                .count() /
            static_cast<f64>(kRuns);
        expect(sink > 0, "the scale benchmark actually propagated something");
        expect(perRunMs < 0.01,
               "32 tags and 40 edges propagate in well under 0.01 ms - inside the noise "
               "floor of the 0.002-0.006 ms evaluation it is added to");
        HBE_INFO("assoctags: EVALUATION cost with associations live - mean {:.5f} ms, "
                 "worst (first eval after bind) {:.4f} ms. Isolated propagation over 32 "
                 "tags / 40 edges: {:.5f} ms per run.",
                 meanEvalMs, worstEvalMs, perRunMs);
    }

    // --- 5. Bake-time validation ---------------------------------------------
    {
        std::vector<TagDef> bad;
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        TagDef a;
        a.name = "A";
        a.associates = {"A", "NoSuchTag", "B"}; // self, missing, and a real cycle partner
        TagDef b;
        b.name = "B";
        b.associates = {"A"};
        TagDef solo;
        solo.name = "Solo";
        solo.associates = {"Elsewhere"}; // defined, but has no shards in this bake
        TagDef elsewhere;
        elsewhere.name = "Elsewhere";
        bad = {untagged, a, b, solo, elsewhere};
        // NOT normalized: the self-reference has to survive to prove the bake reports
        // it (a hand-edited `.hbproj` reaches Bake without passing the editor).
        tags::Reset();
        for (const TagDef& d : bad) tags::Intern(d.name);

        std::vector<tagshard::BakeRow> rows;
        const auto row = [&](const char* tag, f32 x) {
            tagshard::BakeRow r;
            r.tag = tags::Intern(tag);
            r.world = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
            r.localMin = glm::vec3(-0.5f);
            r.localMax = glm::vec3(0.5f);
            r.hasExtent = true;
            r.name = tag;
            rows.push_back(r);
        };
        row("A", 0.0f);
        row("B", 50.0f);
        row("Solo", 100.0f);
        const tagshard::BakeReport rep = tagshard::Bake(rows, bad);
        const auto saw = [&rep](const char* needle) {
            for (const tagshard::Diagnostic& d : rep.diagnostics)
                if (d.message.find(needle) != std::string::npos) return true;
            return false;
        };
        expect(saw("associates ITSELF"), "the bake reports a SELF-association");
        expect(saw("which this project does not list as a tag"),
               "the bake reports an association naming a tag that does not exist");
        expect(saw("Association CYCLE"), "the bake reports a CYCLE");
        expect(saw("A -> B -> A") || saw("B -> A -> B"),
               "and NAMES it, so the finding is actionable");
        expect(saw("no object in this level carries 'Elsewhere'"),
               "the bake distinguishes 'the tag exists but has no content HERE' from "
               "'the tag does not exist' - they look identical at runtime");
        expect(rep.errors == 0,
               "every association finding is a WARNING: none of them refuses the save");
    }

    // --- 6. The `.hbproj` round trip -----------------------------------------
    {
        const fs::path projDir = dir / "P";
        expect(Project::Active().Create(projDir, "P"), "create a scratch project");
        ProjectSettings& s = Project::Active().Settings();
        s.tags = makeDefs();
        s.tags[3].associates = {"City_LowPoly"};
        tags::Normalize(s.tags);
        expect(Project::Active().Save(), "save the project with an association");
        expect(Project::Active().Open(projDir / "P.hbproj"), "reopen it");
        const std::vector<TagDef>& t = Project::Active().Settings().tags;
        expect(t.size() == 4 && t[3].name == "Hill" && t[3].associates.size() == 1 &&
                   t[3].associates[0] == "City_LowPoly",
               "the association round-trips through the .hbproj as a NAME");
        expect(t[1].associates.empty() && t[2].associates.empty(),
               "and a tag with no association parses back as an EMPTY list, not a missing "
               "one (the struct default and the JSON fallback are the same {})");

        // Deleting the TARGET scrubs the dangling name instead of leaving it behind -
        // a string erase, which is exactly why associations are names and not ids.
        std::vector<TagDef> defs2 = Project::Active().Settings().tags;
        Scene sc;
        const usize lowIndex = 2; // City_LowPoly
        expect(defs2[lowIndex].name == "City_LowPoly", "City_LowPoly is where we think it is");
        expect(tags::RemoveTag(sc.Registry(), defs2, lowIndex), "delete City_LowPoly");
        bool dangling = false;
        for (const TagDef& d : defs2)
            for (const std::string& an : d.associates)
                if (an == "City_LowPoly") dangling = true;
        expect(!dangling, "RemoveTag erases the dead name from every other tag's associates");
    }

    // --- 7. A DRIVEN TAG WITH MORE THAN ONE SHARD ------------------------------
    // The configuration every other case in this file misses: every tag above has
    // autoShard off, so every tag has exactly ONE shard - the only shape in which the
    // seed being per-TAG and the hold being per-SHARD cannot disagree.
    //
    // Turn autoShard on for the driven tag and the two facts come apart. The seed is
    // OR'd over a tag's shards, so ONE cell of the low-poly city walking into its own
    // radius made the whole tag "seeded"; with seed and visited sharing one array a
    // seeded tag could never be marked, so every OTHER cell lost `associated` and
    // despawned - while the hill was still driving them. Author-visible as: walk to the
    // near end of the distant city and the far end of it vanishes.
    {
        std::vector<TagDef> wide = makeDefs();
        wide[2].autoShard = true;   // City_LowPoly splits by space
        wide[3].loadRadius = 3000.0f; // a hill you can see the whole valley from
        wide[3].unloadRadius = 3500.0f;
        wide[3].associates.push_back("City_LowPoly");
        tags::Normalize(wide);
        tags::SeedFromProject(wide);
        const TagId wLow = tags::Intern("City_LowPoly"), wHill = tags::Intern("Hill");
        const fs::path wideFile = dir / "WideValley.hbscene";
        {
            Scene s;
            auto& reg = s.Registry();
            const auto make = [&](const char* n, const glm::vec3& p, TagId tag) {
                const entt::entity e = s.CreateEntity(n);
                Transform t;
                t.position = p;
                reg.emplace<Transform>(e, t);
                reg.emplace<AABB>(e, AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});
                tags::Assign(reg, e, tag);
            };
            make("Ground", {0.0f, -1.0f, 0.0f}, kTagUntagged);
            // Two clusters 600 m apart: further than the tag's shard cell (its 60 m
            // load radius), so the bake gives them a shard each.
            for (int i = 0; i < 4; ++i)
                make("LowA", {200.0f + 25.0f * static_cast<f32>(i), 0.0f, 0.0f}, wLow);
            for (int i = 0; i < 4; ++i)
                make("LowB", {900.0f + 25.0f * static_cast<f32>(i), 0.0f, 0.0f}, wLow);
            for (int i = 0; i < 3; ++i)
                make("HillRock", {1990.0f + 10.0f * static_cast<f32>(i), 0.0f, 0.0f}, wHill);
            const tagshard::BakeReport rep = tagshard::BakeScene(s, wide);
            expect(rep.errors == 0, "the wide valley bakes");
            expect(scene::SaveScene(s, wideFile, {}, SceneKind::Full, &rep.shards),
                   "save the wide valley");
        }
        world::Get().Clear();
        world::SetCurrentArea({});
        Scene s;
        Streamer st;
        expect(st.BindLevel(s, renderer, wideFile, dir, wide), "bind the wide valley");
        const i32 iA = st.FindShard("City_LowPoly#0"), iB = st.FindShard("City_LowPoly#1"),
                  iHill = st.FindShard("Hill#0");
        expect(iA >= 0 && iB >= 0 && iHill >= 0,
               "City_LowPoly really baked into TWO shards - without that this test proves "
               "nothing");
        if (iA >= 0 && iB >= 0 && iHill >= 0) {
            // Which key got which cluster is the bake's business, so ask rather than
            // assume: A is the cell the walk below stands in.
            const bool zeroIsNear = st.Shard(static_cast<u32>(iA)).desc.min.x < 500.0f;
            const u32 uA = static_cast<u32>(zeroIsNear ? iA : iB),
                      uB = static_cast<u32>(zeroIsNear ? iB : iA);
            pump(st, s, kFarAway, 12);
            expect(st.ResidentShardCount() == 0, "100 km away nothing is resident");

            pump(st, s, kOnTheHill, 16);
            expect(st.IsResident(uA) && st.IsResident(uB),
                   "standing on the hill pulls in BOTH cells of the low-poly city");
            expect(st.IsAssociated(uA) && st.IsAssociated(uB),
                   "and both know they are held by the association");

            // THE REGRESSION. Walk into cell A. The hill's 3 km radius still reaches
            // here, so it is still driving - and cell B, 650 m away and far outside its
            // own 60 m radius, must still be held.
            const u32 despawnsBefore = st.Stats().despawns;
            const glm::vec3 kInCellA(250.0f, 0.0f, 0.0f);
            pump(st, s, kInCellA, 16);
            expect(st.IsResident(uA), "cell A is now in range on its OWN terms");
            expect(st.IsResident(uB),
                   "and cell B is STILL HELD by the hill, which is still driving - one "
                   "shard of a tag coming into range must not release the tag's others");
            expect(st.IsAssociated(uB),
                   "...and still says so: the mark is per SHARD, and a tag that is both "
                   "seeded and driven is still driven");
            expect(!st.IsSelfHeld(uB), "cell B is here for the association, not for distance");
            expect(st.IsSelfHeld(uA), "while cell A is here on its own distance");
            expect(st.Stats().despawns == despawnsBefore,
                   "nothing was despawned at all on the walk - the bug was a despawn of "
                   "every other shard of the driven tag");

            pump(st, s, kFarAway, 30);
            expect(st.ResidentShardCount() == 0,
                   "and leaving the valley still releases every cell - the wider hold "
                   "leaks nothing");
        }
        worstEvalMs = glm::max(worstEvalMs, st.Stats().maxEvalMs);
    }

    // --- 8. THE SEED'S HYSTERESIS DOES NOT DEPEND ON THE DRIVER BEING RESIDENT --
    // `resident` is false for the whole staging window and forever for a Failed shard,
    // so a seed that borrowed it degenerated to the bare `d <= loadRadius` test with NO
    // dead band - and a focus oscillating on the driver's boundary then spawned and
    // despawned an entire distant city, forever. Here the driver is never allowed to
    // become resident at all (it is forced out on the frames it would have loaded), and
    // the driven tag must still not thrash.
    {
        world::Get().Clear();
        world::SetCurrentArea({});
        Scene s;
        Streamer st;
        std::vector<TagDef> d2 = makeDefs();
        d2[3].associates.push_back("City_LowPoly");
        tags::Normalize(d2);
        expect(st.BindLevel(s, renderer, file, dir, d2), "bind for the boundary walk");
        const i32 iHill = st.FindShard("Hill#0"), iLow = st.FindShard("City_LowPoly#0");
        if (iHill >= 0 && iLow >= 0) {
            // Hill spans x in [1989.5, 2010.5] with load 300 / unload 400. Oscillate ONE
            // FRAME either side of the 300 m boundary, which is the case that used to
            // fail: a shard only becomes `resident` at its finalize, so for the whole
            // staging window - and forever, for a Failed shard - the old seed had no
            // dead band at all and the driven city spawned and despawned on every
            // crossing.
            const glm::vec3 justInside(1989.5f - 299.0f, 0.0f, 0.0f);
            const glm::vec3 justOutside(1989.5f - 301.0f, 0.0f, 0.0f);
            pump(st, s, kFarAway, 16);
            expect(st.ResidentShardCount() == 0, "start the boundary walk with nothing loaded");
            const u32 spawnsBefore = st.Stats().spawns, despawnsBefore = st.Stats().despawns;
            for (int i = 0; i < 30; ++i) {
                pump(st, s, justInside, 1);
                pump(st, s, justOutside, 1);
            }
            expect(st.Stats().despawns == despawnsBefore,
                   "30 crossings of the DRIVER's load boundary cause ZERO despawns of the "
                   "driven tag - the seed carries its own hysteresis and does not borrow "
                   "the driver's residency");
            expect(st.Stats().spawns - spawnsBefore <= 2,
                   "...and at most two spawns happen in total (the hill and the city, once "
                   "each), not one per crossing");
            expect(st.IsResident(static_cast<u32>(iLow)),
                   "and the driven city is still standing at the end of it");
        }
    }

    fs::remove_all(dir, ec);
    world::Get().Clear();
    world::SetCurrentArea({});
    tags::Reset();
    if (ok) {
        HBE_INFO("assoctags: PASS - standing on 'Hill' makes the SEPARATE 'City_LowPoly' "
                 "content resident 1.7 km outside its own radius; leaving releases it "
                 "unless it is in range on its own; two reasons survive losing one; a "
                 "cycle terminates; a missing target warns and streams on.");
    }
    return ok;
}

} // namespace hbe::stream
