// Editor/EditorZoneTest.cpp - `--test-editorzones`: LIVE EDITOR ZONES.
//
// THE CONTRACT, stated once:
//
//   The editor may spawn and despawn streaming zones against the world it is
//   authoring, and NOTHING the author has placed is at risk while it does.
//
// The feature is small; the ways it could destroy a level are not, so the sections
// below are ordered by how much damage the failure would do, worst first:
//
//   * SECTION 3 - A SAVE MUST NEVER WRITE A PARTIAL WORLD. This is the one way this
//     feature could permanently destroy the user's level, and it is silent when it
//     goes wrong: the save-time shard bake re-derives membership from whatever is
//     live, so a file written with three zones despawned is INTERNALLY CONSISTENT and
//     nothing downstream ever reports a problem. Both directions are asserted - the
//     refusal still fires (naming what is missing, with the target file untouched
//     bytes AND mtime), and the settle-then-save path really does produce a complete
//     file, compared against the ORIGINAL file's guids rather than against the
//     registry it was written from.
//
//   * SECTION 1 - THE BIND IS NON-DESTRUCTIVE. scene::BindWorld begins with
//     DestroyWorld, and that one call is the entire reason the editor could not stream
//     before. The proof is not "it looked fine": the live entity set and every guid in
//     it are compared before and after the bind, and Scene::WorldToken - which
//     DestroyWorld bumps and which is what makes Ctrl+S refuse a replaced world - must
//     be UNCHANGED.
//
//   * SECTION 2 - SPAWN/DESPAWN TOUCHES ONLY STREAMED CONTENT, and writes no
//     player-progress state. The authored (untagged) entities and the alwaysLoaded
//     tag's entities keep their guids across a full sweep, and game::SerializeState()
//     - which carries the world:: destroyed/modified diffs a runtime despawn records -
//     is byte-identical before and after, because an authoring bind writes none of it.
//
//   * SECTION 4 - MANUAL OVERRIDES, both directions, and their composition with
//     associations and with alwaysLoaded.
//
//   * SECTION 5 - THE PLAY/STOP SNAPSHOT round-trips with streamed content present.
//     (The snapshot's document half needs an Engine and an ImGui context, so what is
//     driven here is the scene half - which is the half streaming can corrupt.)
//
//   * SECTION 6 - UNDO. A stream event is not an edit and must not enter the stack;
//     an edit taken while zones are despawned must still capture the whole world.
//
// Headless: no GPU (a device-less Renderer returns invalid handles), no window, no
// ImGui context. It builds its own scratch project and level under the temp
// directory and removes them; it never reads or writes the user's project.

#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Game/GameSystems.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/EntityGuid.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/TagShard.h"
#include "Scene/TagStreaming.h"
#include "Scene/TagTable.h"
#include "Scene/WorldState.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hbe {
namespace {

// A file's identity for "nothing was written": bytes AND mtime. Bytes alone would let
// a rewrite-with-identical-content pass as untouched, which is not what a refusal
// promises. (Same shape as SceneSaveTest's, deliberately - it is the same claim.)
struct FileStamp {
    bool exists = false;
    u64 size = 0;
    i64 mtime = 0;
    std::string bytes;
};

FileStamp Stamp(const fs::path& p) {
    FileStamp s;
    std::error_code ec;
    s.exists = fs::exists(p, ec);
    if (!s.exists) return s;
    s.size = static_cast<u64>(fs::file_size(p, ec));
    s.mtime = static_cast<i64>(fs::last_write_time(p, ec).time_since_epoch().count());
    std::ifstream f(p, std::ios::binary);
    s.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return s;
}

bool Same(const FileStamp& a, const FileStamp& b) {
    return a.exists == b.exists && a.size == b.size && a.mtime == b.mtime && a.bytes == b.bytes;
}

// Every guid currently in the registry.
std::set<u64> LiveGuids(const Scene& s) {
    std::set<u64> out;
    const entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<const Guid>()) out.insert(reg.get<const Guid>(e).value);
    return out;
}

usize LiveCount(const Scene& s) {
    usize n = 0;
    for (const entt::entity e : s.Registry().view<entt::entity>())
        if (s.Registry().valid(e)) ++n;
    return n;
}

// Every guid in a parsed `.hbscene` / snapshot string. The census the completeness
// claims are made against - a file compared with itself proves nothing.
std::set<u64> GuidsOfFile(const fs::path& p) {
    std::set<u64> out;
    scene::SceneData d;
    if (!scene::ParseSceneFile(p, d)) return out;
    for (const scene::EntityData& e : d.entities)
        if (e.guid != 0) out.insert(e.guid);
    return out;
}

std::set<u64> GuidsOfString(const std::string& text) {
    std::set<u64> out;
    scene::SceneData d;
    if (!scene::ParseSceneString(text, d)) return out;
    for (const scene::EntityData& e : d.entities)
        if (e.guid != 0) out.insert(e.guid);
    return out;
}

bool HasGuid(const Scene& s, u64 g) {
    const entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<const Guid>())
        if (reg.get<const Guid>(e).value == g) return true;
    return false;
}

entt::entity FindByGuid(const Scene& s, u64 g) {
    const entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<const Guid>())
        if (reg.get<const Guid>(e).value == g) return e;
    return entt::null;
}

} // namespace

bool Editor::EditorZoneSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("editorzones: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_editorzones";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // A SCRATCH PROJECT, because LiveStreamBind resolves the tag list and the assets
    // directory through Project::Active() exactly as the real editor does. The user's
    // project is never opened, read or written by this test.
    if (!Project::Active().Create(dir / "proj", "ZoneTest")) {
        HBE_ERROR("editorzones: FAILED - cannot create the scratch project.");
        fs::remove_all(dir, ec);
        return false;
    }
    const fs::path assetsDir = Project::Active().AssetsDir();
    const fs::path level = assetsDir / "Zones.hbscene";

    // --- The authored level ---------------------------------------------------
    //   Anchor    UNTAGGED        at the origin  - the authored world, never streamed
    //   Perm      alwaysLoaded    at the origin  - a tag with no ShardRuntime at all
    //   Near      load 100        x in [0, 40]
    //   Far       load 100        x around 5000  - ASSOCIATES Vista
    //   Vista     load 50         x around 9000  - only ever reachable by association
    //                                              or by a manual override
    {
        std::vector<TagDef>& defs = Project::Active().Settings().tags;
        defs.clear();
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        TagDef perm;
        perm.name = "Perm";
        perm.alwaysLoaded = true;
        // `near` / `far` are macro-poisoned identifiers on Windows; spelled out.
        TagDef nearTag;
        nearTag.name = "Near";
        nearTag.loadRadius = 100.0f;
        nearTag.unloadRadius = 130.0f;
        TagDef farTag;
        farTag.name = "Far";
        farTag.loadRadius = 100.0f;
        farTag.unloadRadius = 130.0f;
        farTag.associates.push_back("Vista");
        TagDef vista;
        vista.name = "Vista";
        vista.loadRadius = 50.0f;
        vista.unloadRadius = 70.0f;
        defs = {untagged, perm, nearTag, farTag, vista};
        tags::Normalize(defs);
        tags::SeedFromProject(defs);
        expect(defs.size() == 5 && defs[3].name == "Far" && defs[3].associates.size() == 1,
               "the authored tag list survives Normalize with its association intact");
    }
    const std::vector<TagDef>& defs = Project::Active().Settings().tags;
    const TagId tPerm = tags::Intern("Perm"), tNear = tags::Intern("Near"),
                tFar = tags::Intern("Far"), tVista = tags::Intern("Vista");
    u64 gAnchor = 0, gPerm = 0, gNear = 0, gFar = 0, gVista = 0;
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
            return reg.get<Guid>(e).value;
        };
        gAnchor = make("Anchor", {0.0f, 0.0f, 0.0f}, kTagUntagged);
        gPerm = make("PermMarker", {1.0f, 0.0f, 0.0f}, tPerm);
        for (int i = 0; i < 4; ++i) {
            const u64 g = make("NearProp", {10.0f * static_cast<f32>(i), 0.0f, 0.0f}, tNear);
            if (gNear == 0) gNear = g;
        }
        for (int i = 0; i < 4; ++i) {
            const u64 g = make("FarProp", {5000.0f + 10.0f * static_cast<f32>(i), 0.0f, 0.0f}, tFar);
            if (gFar == 0) gFar = g;
        }
        for (int i = 0; i < 3; ++i) {
            const u64 g =
                make("VistaProp", {9000.0f + 10.0f * static_cast<f32>(i), 0.0f, 0.0f}, tVista);
            if (gVista == 0) gVista = g;
        }
        const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
        expect(rep.errors == 0, "the zone level bakes without errors");
        expect(scene::SaveScene(s, level, {}, SceneKind::Full, &rep.shards),
               "save the zone level with its baked shard header");
    }
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    const std::set<u64> fileGuids = GuidsOfFile(level);

    Renderer renderer; // device-less: UploadMesh returns an invalid handle, no GPU
    world::Get().Clear();
    world::SetCurrentArea({});

    // Loads the level the way the EDITOR does - a plain Replace through the scene
    // loader, with no streamer involved - which is the world every section below
    // starts from.
    const auto loadAsEditor = [&](Scene& s) {
        scene::SceneData d;
        scene::ParseSceneFile(level, d);
        scene::StagedAssets staged;
        scene::StageAssets(d, assetsDir, staged);
        scene::Instantiate(s, renderer, d, staged, scene::LoadMode::Replace);
    };
    const auto pump = [&renderer](stream::Streamer& st, Scene& s, const glm::vec3& p, int frames) {
        const std::vector<glm::vec3> foci{p};
        for (int i = 0; i < frames; ++i) st.Update(s, renderer, foci);
    };
    const glm::vec3 kAtOrigin(0.0f, 0.0f, 0.0f);
    const glm::vec3 kAtFar(5000.0f, 0.0f, 0.0f);
    const glm::vec3 kNowhere(500000.0f, 0.0f, 0.0f);

    // ==========================================================================
    // 1. THE BIND IS NON-DESTRUCTIVE
    // ==========================================================================
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s); // the editor loaded it, so this world IS that file

        const usize countBefore = LiveCount(s);
        const std::set<u64> guidsBefore = LiveGuids(s);
        const u64 tokenBefore = s.WorldToken();
        expect(guidsBefore == fileGuids,
               "the editor's plain load really did produce every entity in the file");

        expect(ed.LiveStreamBind(s, renderer), "the editor binds its own streamer to that world");
        expect(ed.liveStream_.IsBound() && ed.liveStream_.Authoring(),
               "the bind is an AUTHORING bind, not a runtime one");
        // THE ASSERTION THIS WHOLE FEATURE RESTS ON. scene::BindWorld's first statement
        // is DestroyWorld; if it had run, all three of these would move.
        expect(LiveCount(s) == countBefore,
               "the bind destroyed NOTHING and spawned NOTHING - the entity count is "
               "identical (BindWorld/DestroyWorld never ran)");
        expect(LiveGuids(s) == guidsBefore,
               "...and every entity is the SAME entity, by guid - nothing was re-minted");
        expect(s.WorldToken() == tokenBefore,
               "...and Scene::WorldToken is unchanged, so Ctrl+S is not refused for "
               "identity (a Replace or a BindWorld would have bumped it)");
        expect(s.Streaming().bound && s.Streaming().authoring,
               "the Scene knows a streamer owns it AND that it is an authoring bind");
        expect(s.Streaming().nonResident == 0, "every zone is resident at the moment of binding");
        expect(ed.liveStream_.ShardCount() == 3,
               "three streamed zones (Near, Far, Vista) - the alwaysLoaded tag has no "
               "ShardRuntime at all and the untagged entity is in no zone");
        expect(ed.liveStream_.FindShard("Perm#0") < 0,
               "an alwaysLoaded tag is not a streamed zone and cannot be forced either way");
        // Streamed vs authored, unambiguously, at all times.
        {
            const auto& reg = s.Registry();
            usize stamped = 0, authored = 0;
            for (const entt::entity e : reg.view<const Guid>()) {
                if (reg.all_of<StreamShard>(e)) ++stamped;
                else ++authored;
            }
            expect(stamped == 11,
                   "every entity of a streamed zone carries the StreamShard membership "
                   "stamp (4 Near + 4 Far + 3 Vista)");
            expect(authored == 2,
                   "and exactly the untagged entity and the alwaysLoaded one do not - "
                   "authored content is distinguishable from streamed content");
        }
        // And unbinding hands the world back exactly as it found it.
        ed.LiveStreamUnbind(s, &renderer);
        expect(!ed.liveStream_.IsBound() && !s.Streaming().bound, "unbind clears the binding");
        expect(LiveCount(s) == countBefore && LiveGuids(s) == guidsBefore,
               "unbinding leaves the world complete and unchanged");
        expect(s.Registry().view<const StreamShard>().size() == 0,
               "and SCRUBS the membership stamps: 'not bound' means 'nothing here is "
               "streamed', so a stale label can never outlive its owner");
    }

    // ==========================================================================
    // 2. LIVE STREAMING SPAWNS AND DESPAWNS WITHOUT TOUCHING AUTHORED ENTITIES
    // ==========================================================================
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the streaming sweep");
        const usize whole = LiveCount(s);
        const std::string gameStateBefore = game::SerializeState();

        ed.liveStream_.SetEnabled(true); // "follow the camera"
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.liveStream_.ResidentShardCount() == 0,
               "500 km away every zone despawns - this is a real spawn/despawn, not a flag");
        expect(LiveCount(s) < whole, "...and the entities are really gone from the registry");
        expect(HasGuid(s, gAnchor),
               "THE AUTHORED (untagged) ENTITY IS UNTOUCHED - streaming may never destroy "
               "something the author placed outside a zone");
        expect(HasGuid(s, gPerm),
               "and so is the alwaysLoaded tag's: it has no ShardRuntime, so nothing can "
               "despawn it");
        expect(!HasGuid(s, gNear) && !HasGuid(s, gFar) && !HasGuid(s, gVista),
               "while all three zones' contents are gone");

        pump(ed.liveStream_, s, kAtOrigin, 24);
        expect(ed.liveStream_.IsResident(static_cast<u32>(ed.liveStream_.FindShard("Near#0"))),
               "returning to the origin respawns the Near zone");
        expect(HasGuid(s, gNear),
               "and its entities come back BY GUID - a respawn restores the same objects, "
               "it does not mint new ones");
        expect(!HasGuid(s, gFar), "the far zone stays out; it is 5 km away");

        pump(ed.liveStream_, s, kAtFar, 24);
        expect(HasGuid(s, gFar), "flying to the far zone spawns it");
        expect(HasGuid(s, gVista),
               "AND its ASSOCIATED zone comes with it, 4 km outside its own load radius - "
               "associations work in the editor exactly as they do at runtime");
        expect(!HasGuid(s, gNear), "while the near zone, now 5 km behind, has gone");

        // AN AUTHORING BIND WRITES NO PLAYER PROGRESS. A runtime despawn records
        // world::CaptureGroup diffs and a respawn replays them; in the editor nothing
        // reverts those writes, and RestoreGroup would re-destroy any member absent at
        // capture time. game::SerializeState() carries that store.
        expect(game::SerializeState() == gameStateBefore,
               "a full despawn/respawn sweep wrote NOTHING into world:: / game:: state - "
               "authoring has no player progress to preserve and must not invent any");

        ed.LiveStreamUnbind(s, &renderer);
        expect(LiveCount(s) == whole && LiveGuids(s) == fileGuids,
               "and after all that, unbinding returns the world to exactly the file - "
               "same count, same guids");
    }

    // ==========================================================================
    // 3. A SAVE MUST NEVER WRITE A PARTIAL WORLD  (the dangerous one)
    // ==========================================================================
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the save tests");

        // (a) THE REFUSAL STILL FIRES FOR AN AUTHORING BIND. This is the clause that
        //     actually protects the file, and StreamingResidency::authoring must not
        //     touch it - a world with holes is silent, total and permanent damage
        //     whoever caused it.
        ed.liveStream_.SetEnabled(true);
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(s.Streaming().nonResident > 0, "zones are despawned");
        const std::string why = scene::SaveRefusal(s);
        expect(!why.empty(),
               "scene::SaveRefusal REFUSES a partially-resident world even when the bind "
               "is an authoring one");
        expect(why.find("#") != std::string::npos, "...and the refusal NAMES what is missing");

        // (b) AND THE REAL SAVE PATH WRITES NOTHING. Asserted on the target file's
        //     BYTES and MTIME: a guard that returns false after touching the file is
        //     not a guard. `ed2` has no binding of its own, so it cannot settle - which
        //     is exactly the situation the refusal exists for.
        {
            Editor ed2;
            ed2.currentScenePath_ = level;
            ed2.AdoptWorld(s);
            const FileStamp before = Stamp(level);
            const bool wrote = ed2.SaveSceneToDisk(s, level);
            const FileStamp after = Stamp(level);
            expect(!wrote, "SaveSceneToDisk REFUSES a partially-resident authoring world");
            expect(Same(before, after),
                   "...and writes NOTHING - the level file is untouched, bytes and mtime");
            expect(!ed2.saveStatus_.empty() && ed2.saveStatusError_,
                   "...and says so as an ERROR the author can see");
        }

        // (c) THE SETTLE-THEN-SAVE PATH. The owning editor brings every zone back
        //     first, so the common case succeeds instead of refusing - and the file it
        //     produces is compared against the ORIGINAL FILE's guids, not against the
        //     registry it was written from (a save that drops the same six entities
        //     every time is internally perfect).
        // A manual override must not be able to survive into the written file either -
        // a zone forced OUT is still content the file describes.
        ed.liveStream_.SetShardForce(0, stream::ShardForce::Unloaded);
        ed.liveStreamAuto_ = true; // "follow the camera" is on when Ctrl+S is pressed
        const bool wrote = ed.SaveSceneToDisk(s, level);
        expect(wrote,
               "the OWNING editor settles every zone resident and the save then succeeds");
        expect(ed.liveStream_.ForcedShardCount() == 0,
               "and the settle dropped the manual overrides - a zone the author forced "
               "OUT may not be written out of the level");
        expect(ed.liveStreamAuto_,
               "...while 'follow the camera' SURVIVES the save: an author who saves every "
               "few minutes must not have to re-enable live mode every few minutes");
        expect(GuidsOfFile(level) == fileGuids,
               "and the written file holds EVERY entity of the original - the zones that "
               "were despawned when Ctrl+S was pressed are all in it");
        expect(s.Streaming().nonResident == 0, "the world really was made whole, not faked");
        expect(ed.liveStream_.IsBound(),
               "and the save RE-BOUND live zones against the file it just wrote (the "
               "shard table was re-baked, so the old binding described a stale level)");
        ed.LiveStreamUnbind(s, &renderer);
    }

    // (d) A RUNTIME BIND IS STILL REFUSED, fully resident or not. This is the
    //     pre-existing guarantee (--test-scenesave asserts it too) and the authoring
    //     flag must not have widened the hole.
    {
        Scene s;
        stream::Streamer rt;
        expect(rt.BindLevel(s, renderer, level, assetsDir, defs, stream::BindMode::Fresh),
               "a RUNTIME (Fresh) bind of the same level");
        for (u32 i = 0; i < rt.ShardCount(); ++i) rt.SpawnShard(s, renderer, i);
        expect(s.Streaming().nonResident == 0 && !s.Streaming().authoring,
               "fully resident, and NOT an authoring bind");
        expect(!scene::SaveRefusal(s).empty(),
               "a fully-resident RUNTIME streamer-owned world is STILL refused - the "
               "authoring flag weakened exactly one clause and only for the editor");
        rt.UnloadAll(s);
        rt.Reset(&s);
    }

    // (e) ...while a fully-resident AUTHORING bind is allowed. The other half of the
    //     same statement, so neither can be changed without the test noticing.
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "an authoring bind, everything resident");
        expect(scene::SaveRefusal(s).empty(),
               "a fully-resident AUTHORING world is NOT refused - this is the one clause "
               "live editor zones reverse");
        ed.LiveStreamUnbind(s, &renderer);
    }

    // ==========================================================================
    // 4. MANUAL OVERRIDES: both directions, and how they compose
    // ==========================================================================
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the override tests");
        const i32 iNear = ed.liveStream_.FindShard("Near#0");
        const i32 iFar = ed.liveStream_.FindShard("Far#0");
        const i32 iVista = ed.liveStream_.FindShard("Vista#0");
        expect(iNear >= 0 && iFar >= 0 && iVista >= 0, "all three zones resolve");
        if (!ok) {
            fs::remove_all(dir, ec);
            return false;
        }
        const u32 uNear = static_cast<u32>(iNear), uFar = static_cast<u32>(iFar),
                  uVista = static_cast<u32>(iVista);

        ed.liveStream_.SetEnabled(true);
        pump(ed.liveStream_, s, kAtOrigin, 24);
        expect(ed.liveStream_.IsResident(uNear) && !ed.liveStream_.IsResident(uFar) &&
                   !ed.liveStream_.IsResident(uVista),
               "at the origin only the Near zone is in range");

        // FORCE IN, beating distance - and DRIVING its associations, which is the
        // useful half: force the vantage point, get the vista.
        ed.liveStream_.SetShardForce(uFar, stream::ShardForce::Resident);
        pump(ed.liveStream_, s, kAtOrigin, 24);
        expect(ed.liveStream_.IsResident(uFar),
               "FORCE RESIDENT spawns a zone 5 km outside its own load radius");
        expect(HasGuid(s, gFar), "...and its objects really are in the registry");
        expect(ed.liveStream_.IsResident(uVista) && ed.liveStream_.IsAssociated(uVista),
               "AND the zone it ASSOCIATES comes with it - a manual override composes "
               "with associations through the same seed set, it is not a second "
               "mechanism bolted alongside");
        expect(ed.liveStream_.ForcedShardCount() == 1,
               "one override is in force, so the panel's banner has something to warn about");

        // FORCE OUT, beating distance the other way - the focus is INSIDE the Near
        // zone's box (distance 0) and it must still go.
        ed.liveStream_.SetShardForce(uNear, stream::ShardForce::Unloaded);
        pump(ed.liveStream_, s, kAtOrigin, 24);
        expect(!ed.liveStream_.IsResident(uNear),
               "FORCE UNLOADED despawns a zone the focus is standing INSIDE - which the "
               "policy's rule 2 would never do, and must never be taught to do");
        expect(!HasGuid(s, gNear), "...and its objects are really gone");
        expect(HasGuid(s, gAnchor) && HasGuid(s, gPerm),
               "authored and alwaysLoaded content is untouched by either override");
        expect(ed.liveStream_.IsSettled({kAtOrigin}),
               "a manually unloaded zone reads as SETTLED - waiting for a zone the author "
               "switched off would hang forever");

        // A FORCED-OUT ZONE STOPS DRIVING. Far is forced in and associates Vista; force
        // Far OUT and Vista must be released too.
        ed.liveStream_.SetShardForce(uFar, stream::ShardForce::Unloaded);
        pump(ed.liveStream_, s, kAtFar, 24);
        expect(!ed.liveStream_.IsResident(uFar),
               "a zone forced OUT stays out even standing inside it");
        expect(!ed.liveStream_.IsResident(uVista),
               "...and it stops DRIVING: the zone it associates is released too");

        // Release everything: the runtime rule takes over again with no residue.
        ed.liveStream_.ClearShardForces();
        expect(ed.liveStream_.ForcedShardCount() == 0, "the overrides are cleared");
        pump(ed.liveStream_, s, kAtFar, 24);
        expect(ed.liveStream_.IsResident(uFar) && ed.liveStream_.IsResident(uVista),
               "with the overrides gone, distance and associations decide again");
        expect(!ed.liveStream_.IsResident(uNear), "and the near zone is genuinely out of range");

        // "Follow the camera" OFF must mean THE WHOLE LEVEL IS HERE, never "half of it
        // is missing and nothing will fix it".
        ed.liveStream_.SetEnabled(false);
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.liveStream_.ResidentShardCount() == ed.liveStream_.ShardCount(),
               "switching automatic streaming OFF loads everything and unloads nothing, "
               "wherever the camera is");
        // ...and a manual override still works in that mode, which is what makes the
        // manual half usable without the automatic half.
        ed.liveStream_.SetShardForce(uNear, stream::ShardForce::Unloaded);
        pump(ed.liveStream_, s, kNowhere, 8);
        expect(!ed.liveStream_.IsResident(uNear),
               "a manual override still applies with automatic streaming switched off");
        ed.LiveStreamUnbind(s, &renderer);
        expect(LiveGuids(s) == fileGuids,
               "and the unbind hands back a complete world despite the override");
    }

    // ==========================================================================
    // 5. THE PLAY/STOP SNAPSHOT ROUND-TRIPS WITH STREAMED CONTENT PRESENT
    // ==========================================================================
    // The snapshot is UNFILTERED, so taken with zones despawned it would bake a world
    // with holes in as the AUTHORED one - and the Replace that restores it would make
    // the holes permanent, because that same Replace unbinds the streamer that would
    // otherwise have refilled them. (The document half of Editor::Snapshot needs an
    // Engine and an ImGui context; the scene half driven here is the half streaming
    // can corrupt.)
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the snapshot round trip");
        ed.liveStream_.SetEnabled(true);
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.liveStream_.ResidentShardCount() == 0, "zones are despawned at the Play edge");

        // What EnterPlayMode does: settle, capture, unbind.
        expect(ed.LiveStreamSettle(s, renderer) == 0, "the capture settles every zone first");
        const std::string snapshot = scene::SaveSceneToString(s);
        expect(GuidsOfString(snapshot) == fileGuids,
               "the Play snapshot holds EVERY entity - nothing that happened to be "
               "streamed out is missing from it");
        ed.LiveStreamUnbind(s, &renderer);
        expect(!ed.liveStream_.IsBound(), "Play does not stream: the binding is handed back");

        // What StopPlayMode does: Replace from the snapshot, then AdoptWorld.
        {
            scene::SceneData d;
            expect(scene::ParseSceneString(snapshot, d), "the snapshot parses back");
            scene::StagedAssets staged;
            scene::StageAssets(d, assetsDir, staged);
            scene::Instantiate(s, renderer, d, staged, scene::LoadMode::Replace);
            ed.AdoptWorld(s);
        }
        expect(LiveGuids(s) == fileGuids,
               "Stop restores the whole world, by guid - a zone that was despawned during "
               "the session is back, not lost");
        expect(s.Registry().view<const StreamShard>().size() == 0,
               "and no membership stamp survives the Replace (they are not serialized), so "
               "nothing is left claiming to be streamed by a streamer that is gone");
        expect(!ed.liveStream_.IsBound(),
               "AdoptWorld unbinds after a world Replace - re-adopting a partially-resident "
               "world would mark despawned zones resident, so this fails closed");
    }

    // ==========================================================================
    // 6. UNDO
    // ==========================================================================
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the undo tests");
        ed.liveStream_.SetEnabled(true);

        // A STREAM EVENT IS NOT AN EDIT. There is no automatic capture anywhere in this
        // editor - no entt signal hook, no per-frame push - and that has to stay true,
        // because an undo stack full of camera movements is an undo stack the author
        // cannot use.
        const usize undoBefore = ed.undoStack_.size();
        pump(ed.liveStream_, s, kNowhere, 24);
        pump(ed.liveStream_, s, kAtOrigin, 24);
        pump(ed.liveStream_, s, kAtFar, 24);
        expect(ed.liveStream_.Stats().despawns > 0 && ed.liveStream_.Stats().spawns > 0,
               "the sweep really did spawn and despawn things");
        expect(ed.undoStack_.size() == undoBefore,
               "and NOT ONE of those spawns or despawns entered the undo stack");

        // AN EDIT TAKEN WHILE ZONES ARE DESPAWNED STILL CAPTURES THE WHOLE WORLD.
        // Otherwise the next Ctrl+Z would restore a world shaped by wherever the camera
        // happened to be when the author last touched something.
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.liveStream_.ResidentShardCount() == 0, "zones are out at the moment of the edit");
        ed.PushUndo(s);
        expect(ed.undoStack_.size() == undoBefore + 1, "the edit pushed one undo step");
        expect(GuidsOfString(ed.undoStack_.back().scene) == fileGuids,
               "and it captured EVERY entity - the push settles the world first, so an "
               "undo can never restore a level with holes");
        expect(ed.liveStream_.ResidentShardCount() == ed.liveStream_.ShardCount(),
               "...which means the world really is whole again after the push");
        ed.LiveStreamUnbind(s, &renderer);
    }

    // ==========================================================================
    // 7. AN EDIT INSIDE A ZONE SURVIVES THE ZONE
    // ==========================================================================
    // The streamer's respawn source used to be the level FILE, parsed at bind. A
    // despawn/respawn cycle was therefore a hard revert-to-file for everything in that
    // zone - and the SAVE ITSELF triggers the respawn (LiveStreamSettle), so Ctrl+S
    // wrote the revert. Worse, an entity CREATED inside the zone is in no file row at
    // all: the despawn's Parent closure destroyed it and nothing could bring it back,
    // with no undo entry, because streamed despawns deliberately never PushUndo.
    //
    // An authoring bind now snapshots the LIVE zone on its way out and respawns from
    // that, so both survive. (This is not world:: - an authoring bind still writes
    // nothing there; see ShardRuntime::authorSnapshot.)
    {
        Scene s;
        loadAsEditor(s);
        Editor ed;
        ed.currentScenePath_ = level;
        ed.AdoptWorld(s);
        expect(ed.LiveStreamBind(s, renderer), "bind for the in-zone edit tests");
        ed.liveStream_.SetEnabled(true);
        auto& reg = s.Registry();

        // (a) MOVE something inside the Near zone.
        const entt::entity moved = FindByGuid(s, gNear);
        expect(moved != entt::null, "the near prop is live to begin with");
        u64 gChild = 0;
        if (moved != entt::null) {
            reg.get<Transform>(moved).position = glm::vec3(7.0f, 3.0f, 0.0f);

            // (b) CREATE a child under it - an entity the FILE has never heard of, and
            // which the despawn closure reaches through Parent.
            const entt::entity child = s.CreateEntity("AuthoredChild");
            Transform ct;
            ct.position = glm::vec3(0.0f, 2.0f, 0.0f);
            reg.emplace<Transform>(child, ct);
            reg.emplace<AABB>(child, AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});
            reg.emplace<Parent>(child, Parent{moved});
            gChild = reg.get<Guid>(child).value;
            expect(gChild != 0, "the authored child has a guid");
            expect(fileGuids.count(gChild) == 0,
                   "...and it is NOT in the file - that is the whole point of the case");
        }

        // Fly away: the near zone streams out, taking both with it.
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.liveStream_.ResidentShardCount() == 0, "the zone really did stream out");
        expect(!HasGuid(s, gNear), "...and its objects are gone from the registry");

        // Come back.
        pump(ed.liveStream_, s, kAtOrigin, 24);
        const entt::entity again = FindByGuid(s, gNear);
        expect(again != entt::null, "the zone respawned");
        if (again != entt::null)
            expect(glm::distance(reg.get<Transform>(again).position,
                                 glm::vec3(7.0f, 3.0f, 0.0f)) < 0.001f,
                   "the MOVE made inside the zone survived the round trip - the respawn "
                   "source is the editor's snapshot, not the file");
        expect(gChild != 0 && HasGuid(s, gChild),
               "and the authored CHILD came back too, guid intact - it is in no file row, "
               "so the file could never have restored it");

        // And a settle (what every save does) sees the edited world, so what reaches the
        // writer is what the author has on screen.
        pump(ed.liveStream_, s, kNowhere, 24);
        expect(ed.LiveStreamSettle(s, renderer) == 0, "settle brings every zone back");
        const entt::entity settled = FindByGuid(s, gNear);
        expect(settled != entt::null &&
                   glm::distance(reg.get<Transform>(settled).position,
                                 glm::vec3(7.0f, 3.0f, 0.0f)) < 0.001f,
               "a save-time settle materialises the EDITED zone, not the file's version - "
               "otherwise the save would write the revert");
        expect(HasGuid(s, gChild), "...including the authored child");
        ed.LiveStreamUnbind(s, &renderer);
    }

    expect(Stamp(level).exists, "the scratch level still exists at the end");

    world::Get().Clear();
    world::SetCurrentArea({});
    fs::remove_all(dir, ec);
    return ok;
}

} // namespace hbe
