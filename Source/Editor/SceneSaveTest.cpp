// Editor/SceneSaveTest.cpp - `--test-scenesave`: the SCENE-SAVE COMPLETENESS CONTRACT.
//
// THE CONTRACT, stated once:
//
//   A `.hbscene` written by Editor::SaveSceneToDisk contains EVERY entity of the
//   active world, or the save does not happen.
//
// Everything below is a way of failing that claim. The two properties that matter
// most, and why each needs its own section:
//
//   * CENSUS AGAINST THE FILE ON DISK (section 1), not against the world in memory.
//     A self-consistency check - "the file I wrote matches the registry I wrote it
//     from" - passes for every bug in this area, because the shard bake, the writer
//     and the loader all re-derive themselves from whatever is live. A save that
//     drops the same six entities on every pass is internally perfect. So the census
//     compares the written file against the ORIGINAL file's guids and keys.
//
//   * TIME INDEPENDENCE (section 2). A save must be a function of the scene, not of
//     how long the editor had it open. The systems that run in edit mode - keyframe
//     tracks, skeletal animation, IK - all write into SERIALIZED fields, so this is
//     the assertion that catches a system baking runtime state over authored state.
//     It is the same failure as the UIAnimator bake, and it is checked by saving the
//     same world after 0 ticks and after 600 and diffing the bytes.
//
// Sections 4-9 are the refusals. A save that cannot be complete must REFUSE and
// write nothing, and "wrote nothing" is asserted on the target file's BYTES, not on
// a return value - a guard that returns false after touching the file is not a guard.
//
// Headless: no GPU (a device-less Renderer returns invalid handles), no window, no
// ImGui context. It takes a scene path and works on a COPY in the temp directory;
// the file it is pointed at is opened read-only and its bytes and mtime are
// re-checked at the end.

#include "Editor/Editor.h"

#include "Core/Log.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/TagShard.h"
#include "Scene/TagStreaming.h"
#include "Scene/TagTable.h"
#include "Scene/TerrainSystem.h"
#include "Scene/WorldState.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hbe {
namespace {

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A file's identity for "nothing was written": bytes AND mtime. Bytes alone would
// let a rewrite-with-identical-content pass as "untouched", which is not what a
// refusal promises.
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
    s.mtime = fs::last_write_time(p, ec).time_since_epoch().count();
    s.bytes = ReadAll(p);
    return s;
}

bool Same(const FileStamp& a, const FileStamp& b) {
    return a.exists == b.exists && a.size == b.size && a.mtime == b.mtime && a.bytes == b.bytes;
}

// EVERY key path a JSON value carries, flattened: "mesh", "mesh.material",
// "ik.chains.target", "terrain.heights". Array INDICES are deliberately not part of
// a path - the union over an array's elements is what "this file contains an ik
// chain with a target" means, and indices would make the comparison depend on
// ordering rather than on content.
//
// TOP-LEVEL KEYS ONLY WAS THE BUG. The census used to record `for (auto& [k, v] :
// entity.items()) keys.insert(k)`, so `mesh` surviving while `mesh.material`
// vanished was invisible - and `mesh.material` vanishing on a missing `.hbmat` is
// precisely one of the losses this test exists to catch. Anything nested inside
// mesh / terrain / ui / particles / rigidBody / characterRig / ik was unchecked.
void FlattenKeys(const json& v, const std::string& prefix, std::set<std::string>& out) {
    if (v.is_object()) {
        for (const auto& [k, sub] : v.items()) {
            const std::string path = prefix.empty() ? k : prefix + "." + k;
            out.insert(path);
            FlattenKeys(sub, path, out);
        }
    } else if (v.is_array()) {
        for (const json& el : v) FlattenKeys(el, prefix, out);
    }
}

// The per-entity census a scene FILE carries: guid -> the set of key PATHS.
// Keyed on guid and never on name: the reference level has two entities called
// "Cube", and a name key collapses them onto one row (the exact mistake
// --test-shardstate was written to correct).
struct Census {
    std::map<u64, std::set<std::string>> byGuid;
    std::set<std::string> rootKeys; // the header: post, tagShards, ambientIntensity...
    usize entities = 0;             // RAW rows, not distinct guids
    usize withoutGuid = 0;
};

Census CensusOf(const scene::SceneData& data, const std::string& text) {
    Census c;
    json doc;
    try {
        doc = json::parse(text);
    } catch (...) {
        return c;
    }
    // THE HEADER IS CONTENT TOO. `tagShards`, `post`, `ambientIntensity`, `exposure`,
    // `giSource`, `shadowDistance` are all authored, and none of them was compared -
    // a save that dropped the entire shard table passed.
    for (const auto& [k, v] : doc.items()) {
        if (k == "entities") continue;
        c.rootKeys.insert(k);
        FlattenKeys(v, k, c.rootKeys);
    }
    const auto ents = doc.find("entities");
    if (ents == doc.end() || !ents->is_array()) return c;
    c.entities = ents->size();
    // The KEYS come from the raw JSON (that is what "the file contains" means), the
    // GUIDS from the parse - because a pre-guid file carries none in its text and
    // ParseSceneFile derives a stable one per row. Both are indexed by row, so they
    // line up as long as the parser reads every row, which it does.
    for (usize i = 0; i < ents->size(); ++i) {
        const u64 g = i < data.entities.size() ? data.entities[i].guid : 0ull;
        if (g == 0) {
            ++c.withoutGuid;
            continue;
        }
        FlattenKeys((*ents)[i], std::string(), c.byGuid[g]);
    }
    return c;
}

// Does `key` name `parent` or anything nested under it.
bool Under(const std::string& key, const char* parent) {
    const usize n = std::strlen(parent);
    return key.compare(0, n, parent) == 0 &&
           (key.size() == n || (key.size() > n && key[n] == '.'));
}

// Keys that a load->save round trip is ALLOWED to drop, each with the reason.
// Anything not on this list disappearing is a FAIL - that is the whole point of
// comparing against the file rather than against the registry.
bool DropAllowed(const std::string& key, const std::set<std::string>& before) {
    // A modular-character ROOT never draws: its MeshRef exists only to give
    // UpdateSkeletal a skeleton, so Instantiate creates no MeshInstance and the
    // writer emits no "mesh". Only legal when the rig asset is real - an entity
    // with an EMPTY Character keeps its mesh (that was F7, and it is asserted in
    // section 5). The whole subtree goes with it.
    if (Under(key, "mesh") && before.count("characterRig")) return true;
    // An IK chain bound to a target ENTITY does not write `target`: anim::UpdateSkeletal
    // recomputes it from that entity every frame, so writing it would store a derived
    // value over the authored one and make the file a function of how long the editor
    // had been open (section 2 would fail). See IKChain in Components.h.
    if (key == "ik.chains.target" && before.count("ik.chains.targetEntity")) return true;
    return false;
}

// Keys a round trip is EXPECTED to add. Additions are the safe direction and are
// NOT a failure - the writer emits every field of every component it writes, so any
// file that predates a field gains it, and forbidding that would fail on every
// legitimate schema addition. But a NEW key appearing is also the shape of a system
// baking derived state into the file on load (the `terrain.heights` case), so
// anything not on this list is WARNED about: visible in the log of every run,
// reviewable, and not a false FAIL.
bool AddExpected(const std::string& key) {
    // Guid stamping: a pre-guid file gets a deterministic guid written on first save.
    if (key == "guid") return true;
    // Sibling order (Scene/Hierarchy.h). ALWAYS emitted, so a file written before the
    // field existed gains it on first save. That is not a load-time bake of derived
    // state: the migration fallback is the entity's FILE ROW INDEX, i.e. exactly the
    // implicit order the file already had, so the added value re-states what was
    // already there rather than inventing anything.
    if (key == "order") return true;
    // The procedural terrain heightfield / splat weights are generated on load and
    // baked out on first save, after which they are authored data.
    if (Under(key, "terrain")) return true;
    // The save-time shard bake stamps each entity's shard index and writes the
    // header table; a level that has never been saved by a shard-aware editor has
    // neither. Also the tag name it is derived from.
    if (key == "shard" || key == "tag" || Under(key, "tagShards")) return true;
    // Component defaults that the writer always emits but an older/hand-written file
    // may not carry. These are VALUES, not references: nothing is lost by adding one.
    if (Under(key, "post") || key == "ambientIntensity" || key == "exposure" ||
        key == "shadowDistance" || key == "version" || key == "giSource")
        return true;
    return false;
}

// Header keys a round trip is allowed to drop.
bool HeaderDropAllowed(const std::string& key) {
    // `tagShards` is re-derived by the SAVE-TIME BAKE, which needs the project's tag
    // table. SaveLikeEditor runs it whenever a project is open (that is what Ctrl+S
    // does), so with `--project` this is never allowed and a dropped shard table is a
    // hard failure. Without one there is nothing to bake from and the table cannot be
    // reproduced - the test says so rather than failing on a missing argument.
    return !Project::HasActive() && Under(key, "tagShards");
}

// THE ROUND TRIP MUST BE THE ONE Ctrl+S PERFORMS. The bare `scene::SaveScene(s, p)`
// writes NO `tagShards` header - the table is a function of the save-time bake, not
// of the registry - so a census taken across it reported the whole shard table as
// lost. Editor::SaveSceneToDisk bakes first and hands the result to SaveScene; so
// does SaveStreamedScenes. Mirroring that here is what makes the header comparison
// (and the fixed-point and time-independence byte comparisons) describe the real
// save rather than a thinner one only the test performs.
bool SaveLikeEditor(Scene& s, const fs::path& out) {
    if (!Project::HasActive()) return scene::SaveScene(s, out);
    const tagshard::BakeReport bake =
        tagshard::BakeScene(s, Project::Active().Settings().tags);
    return scene::SaveScene(s, out, {}, SceneKind::Full, &bake.shards);
}

const char* Yes(bool b) { return b ? "yes" : "NO"; }

// Runs exactly the systems the EDITOR runs on a world at rest. This is the set that
// may not bake anything into the file.
void TickEditMode(Scene& s, Renderer& r, const fs::path& assetsDir, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i) {
        anim::Update(s, 1.0f / 60.0f, /*simulating*/ false);
        terrain::Update(s, r);
        anim::UpdateSkeletal(s, assetsDir, 1.0f / 60.0f, /*simulating*/ false);
    }
}

} // namespace

bool Editor::SceneSaveSelfTest(const fs::path& sceneFile) {
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("scenesave: FAILED - {}", what);
        }
    };

    // --- 0. Arguments, and the promise about the original file ------------------
    // A missing argument FAILS. It must never fall through to "launch the editor"
    // the way --test-uidoc-invariants does - a save test that silently did not run
    // is worse than no save test.
    if (sceneFile.empty()) {
        HBE_ERROR("scenesave: FAILED - no scene given. "
                  "Usage: --test-scenesave <scene.hbscene> [--project <proj>]");
        return false;
    }
    std::error_code ec;
    if (!fs::exists(sceneFile, ec)) {
        HBE_ERROR("scenesave: FAILED - '{}' does not exist.", sceneFile.string());
        return false;
    }
    const FileStamp originalBefore = Stamp(sceneFile);

    const fs::path dir = fs::temp_directory_path(ec) / "hbe_scenesave";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // THE COPY. Every write in this test lands under `dir`; the file the caller
    // named is read exactly once, here, and is re-stamped at the end. A save test
    // that can damage the thing it is testing has no business existing.
    const fs::path work = dir / "Level.hbscene";
    fs::copy_file(sceneFile, work, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        HBE_ERROR("scenesave: FAILED - cannot copy '{}' into the scratch directory: {}",
                  sceneFile.string(), ec.message());
        return false;
    }

    const fs::path assetsDir =
        Project::HasActive() ? Project::Active().AssetsDir() : fs::path();
    if (assetsDir.empty())
        HBE_WARN("scenesave: no --project, so meshes/rigs will not stage. The census and "
                 "the byte comparisons are still exact; only asset-derived state is thinner.");

    // No GPU: a device-less Renderer's UploadMesh/UploadTexture return invalid
    // handles instead of touching a device - the headless contract every other
    // serializer self-test keeps.
    Renderer renderer;

    // --- 1. CENSUS: the written file against the file on disk -------------------
    scene::SceneData a;
    expect(scene::ParseSceneFile(work, a), "the level parses");
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    const Census censusA = CensusOf(a, ReadAll(work));
    expect(censusA.entities > 0, "the level is not empty");
    expect(censusA.withoutGuid == 0,
           "every row resolves to a stable guid (ParseSceneFile derives one for a "
           "pre-guid file, so this holds for old levels too)");

    const fs::path outB0 = dir / "B0.hbscene";
    {
        Scene s;
        scene::StagedAssets staged;
        scene::StageAssets(a, assetsDir, staged);
        scene::Instantiate(s, renderer, a, staged, scene::LoadMode::Replace);
        expect(SaveLikeEditor(s, outB0), "save the freshly loaded world (0 ticks)");
    }
    // SaveScene writes aside and renames, so a failed write cannot truncate the
    // target. A successful one must leave no debris behind.
    expect(!fs::exists(fs::path(outB0.string() + ".tmp"), ec),
           "a successful save leaves no .tmp file next to the scene");
    scene::SceneData b0;
    expect(scene::ParseSceneFile(outB0, b0), "the saved level parses back");
    const Census censusB = CensusOf(b0, ReadAll(outB0));

    // 1a. NO OBJECT MAY VANISH. Multiset equality on guids.
    {
        std::vector<u64> lost, gained;
        for (const auto& [g, keys] : censusA.byGuid)
            if (!censusB.byGuid.count(g)) lost.push_back(g);
        for (const auto& [g, keys] : censusB.byGuid)
            if (!censusA.byGuid.count(g)) gained.push_back(g);
        for (const u64 g : lost) {
            // Name it, so a failure is actionable rather than a count.
            std::string name;
            for (const scene::EntityData& d : a.entities)
                if (d.guid == g) name = d.name;
            HBE_ERROR("scenesave: LOST entity {:016x} '{}' - it is in the source file and "
                      "not in the saved file.",
                      g, name);
        }
        expect(lost.empty(), "load -> save must not DROP a single entity");
        expect(gained.empty(), "load -> save must not INVENT an entity");
        expect(censusA.byGuid.size() == censusB.byGuid.size(),
               "the entity census is unchanged in size");
        // RAW ROW COUNT, alongside the guid multiset. Two rows sharing one guid
        // collapse onto a single map entry, so dropping the duplicate is invisible to
        // every assertion above.
        expect(censusA.entities == censusB.entities,
               "the number of entity ROWS in the file is unchanged (a guid collision "
               "would hide a dropped duplicate from the census above)");
    }

    // 1b. NO COMPONENT MAY VANISH - at ANY DEPTH. Key PATHS present in the source
    //     file must still be present, except for documented, whitelisted
    //     normalizations. `mesh` surviving while `mesh.material` vanished used to
    //     pass; it is the loss a missing `.hbmat` causes.
    //
    //     New keys are not a failure (the writer emits every field of every component,
    //     so any file predating a field gains it) but unexpected ones are WARNED
    //     about, because a new key is also what a load-time bake looks like.
    {
        u32 lostKeys = 0;
        std::set<std::string> addedUnexpected;
        for (const auto& [g, before] : censusA.byGuid) {
            const auto it = censusB.byGuid.find(g);
            if (it == censusB.byGuid.end()) continue; // already reported above
            for (const std::string& k : before) {
                if (it->second.count(k)) continue;
                if (DropAllowed(k, before)) continue;
                std::string name;
                for (const scene::EntityData& d : a.entities)
                    if (d.guid == g) name = d.name;
                HBE_ERROR("scenesave: {:016x} '{}' LOST key '{}'", g, name, k);
                ++lostKeys;
            }
            for (const std::string& k : it->second)
                if (!before.count(k) && !AddExpected(k)) addedUnexpected.insert(k);
        }
        expect(lostKeys == 0, "no component key present in the source file may be dropped");
        for (const std::string& k : addedUnexpected)
            HBE_WARN("scenesave: the round trip ADDED key '{}' (not a failure - but if "
                     "a system is baking derived state into the file on load, this is "
                     "what it looks like).",
                     k);
    }

    // 1b'. THE HEADER. tagShards / post / ambientIntensity / exposure / giSource are
    //      authored content that lives outside `entities`, and none of it was
    //      compared - a save that dropped the whole shard table passed.
    {
        u32 lostRoot = 0;
        for (const std::string& k : censusA.rootKeys) {
            if (censusB.rootKeys.count(k)) continue;
            if (HeaderDropAllowed(k)) continue;
            HBE_ERROR("scenesave: the scene HEADER lost key '{}'", k);
            ++lostRoot;
        }
        expect(lostRoot == 0, "no header key present in the source file may be dropped");
    }

    // 1c. The two things the reference level exists to exercise here.
    {
        bool sawTerrain = false, sawSixParts = false;
        u32 parts = 0;
        for (const scene::EntityData& d : b0.entities) {
            if (d.hasTerrain) sawTerrain = true;
            if (d.name.rfind("Ch22_", 0) == 0) ++parts;
        }
        sawSixParts = parts >= 6;
        HBE_INFO("scenesave: census {} entities; Terrain present: {}; Ch22_* parts: {}",
                 censusB.byGuid.size(), Yes(sawTerrain), parts);
        // Only asserted when the level actually has them, so the test stays usable on
        // any scene - but it SAYS what it found, so running it on the wrong file is
        // visible rather than silently vacuous.
        if (censusA.byGuid.size() >= 18) {
            expect(sawTerrain, "the Terrain survives the round trip");
            expect(sawSixParts, "all six Ch22_* character parts survive the round trip");
        }
    }

    // --- 2. TIME INDEPENDENCE: 0 ticks and 600 ticks must write the same bytes ---
    const fs::path outB600 = dir / "B600.hbscene";
    {
        Scene s;
        scene::StagedAssets staged;
        scene::StageAssets(a, assetsDir, staged);
        scene::Instantiate(s, renderer, a, staged, scene::LoadMode::Replace);
        TickEditMode(s, renderer, assetsDir, 600);
        // THE EXCLUSION TABLE, checked against a world where the excluded entities
        // actually EXIST. Reported here rather than at 0 ticks because that is the
        // point: terrain chunks, world-UI quads and character parts are created BY the
        // systems this loop runs, so a settled world is the only place the claim
        // "something else puts it back" can be observed at all. Every row printed here
        // is an entity the file does not contain - and the byte comparison immediately
        // below is the proof that leaving it out cost nothing.
        {
            std::map<std::string, u32> excluded;
            for (const entt::entity e : s.Registry().view<entt::entity>()) {
                if (!s.Registry().valid(e)) continue;
                const char* regen = nullptr;
                if (const char* comp = scene::SceneWriteExclusion(s.Registry(), e, &regen))
                    ++excluded[comp];
            }
            HBE_INFO("scenesave: the exclusion table has {} row(s); {} of them are live in "
                     "this level after 600 ticks:",
                     scene::SceneWriteExclusionCount(), excluded.size());
            for (const auto& [comp, n] : excluded) {
                const char* regen = nullptr;
                for (usize i = 0; i < scene::SceneWriteExclusionCount(); ++i) {
                    const char* row = scene::SceneWriteExclusionRow(i, &regen);
                    if (row && comp == row) break;
                }
                HBE_INFO("scenesave:   {:>4} x {} - EXCLUDED because {}", n, comp,
                         regen ? regen : "(no reason recorded)");
            }
            // NOTE on why this list is usually empty here: terrain::Update returns
            // immediately without a device (Renderer::SupportsScene), so chunk
            // regeneration - and with it the TerrainChunk rows - cannot be observed
            // headlessly. Section 4b below therefore checks every row of the table
            // DIRECTLY, by putting one live entity of each kind in a scene and
            // asserting the writer leaves exactly those out.
        }
        expect(SaveLikeEditor(s, outB600), "save the same world after 600 edit-mode ticks");
    }
    expect(ReadAll(outB0) == ReadAll(outB600),
           "a save is a function of the SCENE, not of how long the editor had it open "
           "(0 ticks vs 600 ticks must be byte-identical)");

    // --- 3. FIXED POINT: load the saved file, save it again, byte-identical ------
    const fs::path outC = dir / "C.hbscene";
    {
        scene::SceneData bd;
        expect(scene::ParseSceneFile(outB0, bd), "the saved level re-parses");
        Scene s;
        scene::StagedAssets staged;
        scene::StageAssets(bd, assetsDir, staged);
        scene::Instantiate(s, renderer, bd, staged, scene::LoadMode::Replace);
        expect(SaveLikeEditor(s, outC), "save the reloaded world");
    }
    expect(ReadAll(outB0) == ReadAll(outC),
           "save -> load -> save is a FIXED POINT (B and C byte-identical)");

    // --- 4. THE HAZARDS, synthesised ---------------------------------------------
    // Sections 1-3 run on the real level, which happens to dodge every edit-mode bake
    // (its one keyframe track is paused and all three animators loop). That is luck,
    // not design, so the hazards are built here explicitly: a non-looping track that
    // is authored PLAYING, an IK chain bound to a target ENTITY, and a mesh entity
    // carrying an EMPTY Character. Each was a real loss path.
    {
        const fs::path hz = dir / "Hazards.hbscene";
        u64 gTrack = 0, gIk = 0, gMesh = 0;
        {
            Scene s;
            auto& reg = s.Registry();

            const entt::entity track = s.CreateEntity("AutoPlayDoor");
            Transform txf;
            txf.position = {7.0f, 8.0f, 9.0f}; // authored pose, must survive
            reg.emplace<Transform>(track, txf);
            AnimationTrack at;
            at.duration = 2.0f;
            at.loop = false;   // runs out
            at.playing = true; // authored to auto-play
            at.speed = 1.0f;
            AnimationTrack::Key k0, k1;
            k0.time = 0.0f;
            k0.position = {1.0f, 0.0f, 0.0f};
            k1.time = 2.0f;
            k1.position = {5.0f, 0.0f, 0.0f};
            at.keys = {k0, k1};
            reg.emplace<AnimationTrack>(track, at);

            const entt::entity anchor = s.CreateEntity("IkAnchor");
            Transform axf;
            axf.position = {40.0f, 1.0f, 0.0f};
            reg.emplace<Transform>(anchor, axf);

            const entt::entity ik = s.CreateEntity("IkUser");
            reg.emplace<Transform>(ik);
            IKConstraint ikc;
            IKChain chain;
            chain.endJoint = "Hand_L";
            chain.targetEntity = "IkAnchor"; // derived every frame from the anchor
            chain.target = {0.0f, 0.0f, 0.0f};
            ikc.chains.push_back(chain);
            reg.emplace<IKConstraint>(ik, ikc);

            // F7: a mesh + an EMPTY Character. One Inspector "Add Component" away,
            // and the reference level's Terrain already carries the empty rig.
            const entt::entity mesh = s.CreateEntity("MeshWithEmptyRig");
            reg.emplace<Transform>(mesh);
            reg.emplace<MeshRef>(mesh, MeshRef{"prim:cube"});
            MeshInstance mi;
            mi.surface.base_color = {0.1f, 0.2f, 0.3f, 1.0f};
            mi.surface.specular_roughness = 0.77f;
            reg.emplace<MeshInstance>(mesh, mi);
            reg.emplace<Character>(mesh, Character{}); // asset == ""

            expect(scene::SaveScene(s, hz), "author the hazard scene");
            gTrack = reg.get<Guid>(track).value;
            gIk = reg.get<Guid>(ik).value;
            gMesh = reg.get<Guid>(mesh).value;
        }

        const auto roundTrip = [&](u32 ticks, const fs::path& out) {
            scene::SceneData d;
            if (!scene::ParseSceneFile(hz, d)) return;
            Scene s;
            scene::StagedAssets staged;
            scene::StageAssets(d, assetsDir, staged);
            scene::Instantiate(s, renderer, d, staged, scene::LoadMode::Replace);
            TickEditMode(s, renderer, assetsDir, ticks);
            scene::SaveScene(s, out);
        };
        const fs::path h0 = dir / "H0.hbscene", h600 = dir / "H600.hbscene";
        roundTrip(0, h0);
        roundTrip(600, h600);
        expect(ReadAll(h0) == ReadAll(h600),
               "the HAZARD scene is time-independent too: an auto-playing non-looping "
               "track, an entity-bound IK chain and an empty Character must not bake "
               "runtime state into the file");

        scene::SceneData h;
        expect(scene::ParseSceneFile(h600, h), "the hazard round trip parses");
        for (const scene::EntityData& d : h.entities) {
            if (d.guid == gTrack) {
                expect(d.anim.playing,
                       "an authored 'plays on load' track must still say so after the "
                       "clip has run out in the editor");
                // The written transform is the pose the entity WILL hold on the frame
                // after this file loads (t = 0), not wherever the preview playhead
                // stopped - which is what makes the file a fixed point.
                expect(std::abs(d.transform.position.x - 1.0f) < 1e-4f,
                       "the transform written for an auto-playing track is its t=0 pose, "
                       "not the end-of-clip pose the preview left behind");
            }
            if (d.guid == gIk) {
                expect(d.hasIK && !d.ik.chains.empty() &&
                           d.ik.chains[0].targetEntity == "IkAnchor",
                       "the IK chain and its target ENTITY survive");
            }
            if (d.guid == gMesh) {
                expect(d.hasMesh && d.meshSource == "prim:cube",
                       "a mesh entity carrying an EMPTY Character keeps its mesh (an "
                       "empty rig spawns nothing, so suppressing the MeshInstance lost "
                       "the reference permanently)");
                expect(std::abs(d.roughness - 0.77f) < 1e-4f,
                       "...and its inline material values with it");
            }
        }
    }

    // --- 4b. EVERY ROW OF THE EXCLUSION TABLE, EXERCISED -------------------------
    // The table is a list of claims ("this entity is left out because X puts it back").
    // Here each row gets one live entity, alongside two ordinary authored ones, and the
    // written file must contain the ordinary two and nothing else. A row that stops
    // matching - a component renamed, a predicate inverted - shows up as an entity
    // appearing in a file that should not have it, which is the direction that costs.
    {
        const fs::path ex = dir / "Exclusions.hbscene";
        Scene s;
        auto& reg = s.Registry();
        const auto plain = [&](const char* n) {
            const entt::entity e = s.CreateEntity(n);
            reg.emplace<Transform>(e);
            return e;
        };
        const entt::entity keepA = plain("KeepMe");
        const entt::entity keepB = plain("KeepMeToo");
        const entt::entity terrainRoot = plain("TerrainRoot");
        reg.emplace<TerrainChunk>(plain("Chunk_0_0"), TerrainChunk{0, 0});
        reg.emplace<UISurface>(plain("WorldUIQuad"), UISurface{});
        reg.emplace<DialogueChoiceButton>(plain("Choice0"), DialogueChoiceButton{});
        reg.emplace<InteractPromptTag>(plain("Prompt"));
        reg.emplace<SkinnedPartRef>(plain("Part"), SkinnedPartRef{terrainRoot, "s", "v"});
        reg.emplace<DebrisChunk>(plain("Debris"), DebrisChunk{});
        reg.emplace<Persistent>(plain("ResidentUI"));
        reg.emplace<UIDocMember>(plain("MenuButton"), UIDocMember{});
        expect(scene::SaveScene(s, ex), "save a scene holding one entity per exclusion row");

        scene::SceneData d;
        expect(scene::ParseSceneFile(ex, d), "the exclusion scene parses");
        std::set<std::string> written;
        for (const scene::EntityData& e : d.entities) written.insert(e.name);
        expect(written.size() == 3 && written.count("KeepMe") && written.count("KeepMeToo") &&
                   written.count("TerrainRoot"),
               "the writer wrote EXACTLY the entities no exclusion row matches");
        // And every row is a row the predicate actually reaches - a table entry that
        // matched nothing would make the assertion above pass for the wrong reason.
        u32 matched = 0;
        for (const entt::entity e : reg.view<entt::entity>())
            if (reg.valid(e) && scene::SceneWriteExclusion(reg, e, nullptr)) ++matched;
        expect(matched == scene::SceneWriteExclusionCount(),
               "every row of the exclusion table matched exactly one of these entities "
               "(no row is dead, none is duplicated)");
        expect(reg.valid(keepA) && reg.valid(keepB), "the plain entities are untouched");
    }

    // --- 4c. A MISSING ASSET MUST NOT DELETE THE REFERENCE TO IT ----------------
    // The single worst shape in this file, and it had TWO live instances that the
    // top-level-keys-only census could not see:
    //
    //   * a `.hbmat` that fails to stage (renamed, deleted, unparseable) meant no
    //     MaterialRef, and the writer emits `mesh.material` only when that component
    //     exists - so open the level, Ctrl+S, and the assignment is GONE. Restoring
    //     the asset afterwards leaves nothing pointing at it.
    //   * a `.hbpaint` that fails to load meant no PaintComponent at all, so the
    //     whole `paint` key went: source, resolution, relief, opacity, heightScale,
    //     lodBias, layer, projection.
    //
    // Both now survive as REFERENCES with the asset absent (the object simply does
    // not draw/paint until it comes back), which is what the mesh half already did.
    {
        const fs::path miss = dir / "MissingAssets.hbscene";
        u64 gMat = 0, gPaint = 0;
        {
            Scene s;
            auto& reg = s.Registry();

            const entt::entity m = s.CreateEntity("PropWithLostMaterial");
            reg.emplace<Transform>(m);
            reg.emplace<MeshRef>(m, MeshRef{"prim:cube"});
            reg.emplace<MeshInstance>(m, MeshInstance{});
            // A path that cannot exist under any Assets/ root.
            reg.emplace<MaterialRef>(m, MaterialRef{"Materials/__hbe_missing__.hbmat"});

            const entt::entity p = s.CreateEntity("PaintedPropWithLostCanvas");
            reg.emplace<Transform>(p);
            PaintComponent pc;
            pc.source = "Paint/__hbe_missing__.hbpaint";
            pc.resolution = 2048;
            pc.enabled = false;
            pc.locked = true;
            pc.reliefEnabled = false;
            pc.opacity = 0.5f;
            pc.heightScale = 0.33f;
            pc.lodBias = 2.0f;
            pc.layer = "Rust";
            pc.projection = 1;
            reg.emplace<PaintComponent>(p, std::move(pc));

            expect(scene::SaveScene(s, miss), "author a scene referencing two absent assets");
            gMat = reg.get<Guid>(m).value;
            gPaint = reg.get<Guid>(p).value;
        }

        scene::SceneData src;
        expect(scene::ParseSceneFile(miss, src), "the missing-asset scene parses");
        const Census before = CensusOf(src, ReadAll(miss));
        expect(before.byGuid.count(gMat) && before.byGuid.at(gMat).count("mesh.material"),
               "the authored scene really does carry mesh.material");
        expect(before.byGuid.count(gPaint) && before.byGuid.at(gPaint).count("paint.source"),
               "...and paint.source");

        const fs::path missOut = dir / "MissingAssetsOut.hbscene";
        {
            Scene s;
            scene::StagedAssets staged;
            scene::StageAssets(src, assetsDir, staged); // both loads WARN and give up
            scene::Instantiate(s, renderer, src, staged, scene::LoadMode::Replace);
            expect(scene::SaveScene(s, missOut), "save the world with both assets absent");
        }
        scene::SceneData outD;
        expect(scene::ParseSceneFile(missOut, outD), "it parses back");
        const Census after = CensusOf(outD, ReadAll(missOut));
        u32 lostHere = 0;
        for (const u64 g : {gMat, gPaint}) {
            const auto ba = before.byGuid.find(g), bb = after.byGuid.find(g);
            if (ba == before.byGuid.end() || bb == after.byGuid.end()) {
                HBE_ERROR("scenesave: entity {:016x} vanished with its asset", g);
                ++lostHere;
                continue;
            }
            for (const std::string& k : ba->second)
                if (!bb->second.count(k) && !DropAllowed(k, ba->second)) {
                    HBE_ERROR("scenesave: {:016x} LOST key '{}' because the asset it "
                              "names is not on disk",
                              g, k);
                    ++lostHere;
                }
        }
        expect(lostHere == 0,
               "a missing .hbmat / .hbpaint must not delete the reference to it - every "
               "authored key survives so the link is restorable");
        // The values, not just the key names: the paint metadata lives in the SCENE,
        // so all of it is recoverable without the canvas file.
        for (const scene::EntityData& d : outD.entities) {
            if (d.guid != gPaint) continue;
            expect(d.hasPaint && d.paintSource == "Paint/__hbe_missing__.hbpaint" &&
                       d.paintResolution == 2048 && !d.paintEnabled && d.paintLocked &&
                       !d.paintReliefEnabled && d.paintLayer == "Rust" &&
                       d.paintProjection == 1,
                   "every authored paint setting round-trips with the canvas absent");
        }
        for (const scene::EntityData& d : outD.entities) {
            if (d.guid != gMat) continue;
            expect(d.materialAsset == "Materials/__hbe_missing__.hbmat",
                   "the material link round-trips with the .hbmat absent");
        }
    }

    // --- 5. THE SUBTREE WRITER USES THE SAME EXCLUSION TABLE ---------------------
    // Ctrl+D / "Save as Prefab" go through BuildSubtreeJson, whose skip list used to
    // be a hand-written subset. A modular character's spawned parts have no MeshRef,
    // so they copied as empty named entities and pasted back as junk - which the next
    // Ctrl+S then wrote into the .hbscene as real authored content.
    {
        Scene s;
        auto& reg = s.Registry();
        const entt::entity root = s.CreateEntity("CharRoot");
        reg.emplace<Transform>(root);
        for (int i = 0; i < 6; ++i) {
            const entt::entity part = s.CreateEntity("Slot" + std::to_string(i));
            reg.emplace<Transform>(part);
            reg.emplace<Parent>(part, Parent{root});
            reg.emplace<SkinnedPartRef>(part, SkinnedPartRef{root, "slot", "variant"});
        }
        const entt::entity plain = s.CreateEntity("RealChild");
        reg.emplace<Transform>(plain);
        reg.emplace<Parent>(plain, Parent{root});

        scene::SceneData frag;
        expect(scene::ParseSceneString(scene::SaveSubtreeToString(s, root), frag),
               "the subtree fragment parses");
        expect(frag.entities.size() == 2,
               "a duplicated character root carries its REAL children and none of its "
               "six respawned parts");
    }

    // --- 6. A SAVE WITH DESPAWNED SHARDS IS REFUSED, AND WRITES NOTHING ----------
    {
        const fs::path lvl = dir / "Streamed.hbscene";
        std::vector<TagDef> defs;
        {
            TagDef untagged;
            untagged.name = tags::kUntaggedName;
            TagDef near_;
            near_.name = "NearProps";
            near_.loadRadius = 50.0f;
            TagDef farTag; // not `far`: windows.h still #defines that legacy keyword
            farTag.name = "FarProps";
            farTag.loadRadius = 50.0f;
            defs = {untagged, near_, farTag};
            tags::Normalize(defs);
            tags::SeedFromProject(defs);
        }
        const TagId tNear = tags::Intern("NearProps"), tFar = tags::Intern("FarProps");
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
            make("Anchor", {0.0f, 0.0f, 0.0f}, kTagUntagged);
            make("NearA", {100.0f, 0.0f, 0.0f}, tNear);
            make("NearB", {102.0f, 0.0f, 0.0f}, tNear);
            make("FarA", {900.0f, 0.0f, 0.0f}, tFar);
            make("FarB", {902.0f, 0.0f, 0.0f}, tFar);
            const tagshard::BakeReport rep = tagshard::BakeScene(s, defs);
            expect(rep.errors == 0, "the streamed test level bakes cleanly");
            expect(scene::SaveScene(s, lvl, {}, SceneKind::Full, &rep.shards),
                   "save the streamed test level with its shard header");
        }
        world::Get().Clear();
        world::SetCurrentArea({});

        Scene s;
        stream::Streamer st;
        expect(st.BindLevel(s, renderer, lvl, dir, defs), "bind the streamed level");
        expect(st.ShardCount() == 2, "two streamed shards");

        // At bind, streamed shards are UNLOADED: the world holds the resident slice
        // only. This IS the dangerous state, and it is the state the old code could
        // reach inside the editor.
        expect(s.Streaming().bound && s.Streaming().nonResident == 2,
               "the Scene knows its world is streamed and incomplete");
        expect(!scene::SaveRefusal(s).empty(),
               "scene::SaveRefusal refuses a world with despawned shards");
        expect(scene::SaveRefusal(s).find("NearProps#0") != std::string::npos ||
                   scene::SaveRefusal(s).find("FarProps#0") != std::string::npos,
               "the refusal NAMES the shards that are missing");

        // And the real save path, end to end: SaveSceneToDisk must return false and
        // leave the file byte-for-byte and mtime-for-mtime untouched.
        {
            Editor ed;
            ed.currentScenePath_ = lvl;
            const FileStamp before = Stamp(lvl);
            const bool wrote = ed.SaveSceneToDisk(s, lvl);
            const FileStamp after = Stamp(lvl);
            expect(!wrote, "SaveSceneToDisk REFUSES a partially-resident streamed world");
            expect(Same(before, after),
                   "...and writes NOTHING - the level file is untouched, bytes and mtime");
            expect(!ed.saveStatus_.empty() && ed.saveStatusError_,
                   "...and the refusal is reported as an ERROR the author can see");
        }

        // Every shard resident is still refused: a streamer-owned world's contents are
        // a function of the camera, and authoring belongs to the editor's own load
        // path. Refusing is cheap here; being wrong is not.
        for (u32 i = 0; i < st.ShardCount(); ++i) st.SpawnShard(s, renderer, i);
        expect(s.Streaming().nonResident == 0, "all shards spawned");
        expect(!scene::SaveRefusal(s).empty(),
               "a fully-resident STREAMER-OWNED world is still refused");

        // Unbinding hands the world back.
        st.UnloadAll(s);
        st.Reset(&s);
        expect(!s.Streaming().bound, "Reset clears the Scene's streaming summary");
    }

    // --- 7. A WORLD REPLACED BEHIND THE EDITOR'S BACK IS REFUSED ----------------
    // The shape of the dev-menu checkpoint load: something Replace-loads a different
    // world into the registry while the editor goes on believing currentScenePath_
    // describes it. The editor no longer HAS that door (the overlay is runtime-only
    // now), but the guard is what makes that a defence in depth rather than a hope.
    {
        const fs::path levelA = dir / "WorldA.hbscene";
        const fs::path levelB = dir / "WorldB.hbscene";
        for (const auto& [p, n] : {std::pair{levelA, "FromA"}, std::pair{levelB, "FromB"}}) {
            Scene s;
            const entt::entity e = s.CreateEntity(n);
            s.Registry().emplace<Transform>(e);
            expect(scene::SaveScene(s, p), "author a world for the identity test");
        }

        Scene s;
        Editor ed;
        expect(scene::LoadScene(s, renderer, levelA), "load world A");
        ed.currentScenePath_ = levelA;
        ed.AdoptWorld(s); // "this registry IS levelA"

        // Something else Replace-loads a different world into the same Scene.
        expect(scene::LoadScene(s, renderer, levelB), "another system replaces the world");

        const FileStamp before = Stamp(levelA);
        const bool wrote = ed.SaveSceneToDisk(s, levelA);
        const FileStamp after = Stamp(levelA);
        expect(!wrote, "a world that was replaced is REFUSED against the old path");
        expect(Same(before, after), "...and world A's file is untouched");

        // Re-adopting (what a real editor load does) makes it saveable again, so the
        // guard is a guard and not a lock.
        ed.AdoptWorld(s);
        ed.currentScenePath_ = levelB;
        expect(ed.SaveSceneToDisk(s, levelB), "the adopted world saves to its own path");
    }

    // --- 8. AN EMPTY WORLD OVER A NON-EMPTY FILE IS REFUSED ---------------------
    // This used to write nothing, return TRUE, and report "Saved scene 'X'" - so
    // select-all + Delete + Ctrl+S claimed success and the objects came back on the
    // next load. Neither half was acceptable: it lied, and it made a legitimately
    // emptied scene unsaveable.
    {
        const fs::path file = dir / "Emptied.hbscene";
        {
            Scene s;
            for (const char* n : {"Keep1", "Keep2"}) {
                const entt::entity e = s.CreateEntity(n);
                s.Registry().emplace<Transform>(e);
            }
            expect(scene::SaveScene(s, file), "author a populated scene");
        }
        Scene empty;
        Editor ed;
        ed.currentScenePath_ = file;
        const FileStamp before = Stamp(file);
        const bool wrote = ed.SaveSceneToDisk(empty, file);
        const FileStamp after = Stamp(file);
        expect(!wrote, "an EMPTY world is refused over a file that holds objects");
        expect(Same(before, after), "...and the populated file is untouched");
        expect(ed.saveStatusError_ && ed.saveStatus_.find("EMPTY") != std::string::npos,
               "...and says so");

        // Over a file that does not exist there is nothing to lose, so it writes.
        const fs::path fresh = dir / "FreshEmpty.hbscene";
        Editor ed2;
        ed2.currentScenePath_ = fresh;
        expect(ed2.SaveSceneToDisk(empty, fresh),
               "an empty world DOES save when there is no file to overwrite");
    }

    // --- 9. PLAY MODE ------------------------------------------------------------
    // The Play world is a snapshot-restored copy that gameplay destroys entities in
    // and strips components from. It may never be written over the authored file.
    {
        const fs::path file = dir / "Playing.hbscene";
        Scene s;
        const entt::entity e = s.CreateEntity("Prop");
        s.Registry().emplace<Transform>(e);
        expect(scene::SaveScene(s, file), "author a scene to play");
        Editor ed;
        ed.currentScenePath_ = file;
        ed.playMode_ = true;
        const FileStamp before = Stamp(file);
        const bool wrote = ed.SaveSceneToDisk(s, file);
        const FileStamp after = Stamp(file);
        expect(!wrote, "SaveSceneToDisk refuses while PLAYING");
        expect(Same(before, after), "...and writes nothing");
        ed.playMode_ = false;
        expect(ed.SaveSceneToDisk(s, file), "...and saves again once Play stops");
    }

    // --- 9b. A PREVIEW IS POSING THE SCENE --------------------------------------
    // Cutscene preview and the movie renderer are destructive in exactly the way
    // Play is: cutscene::Evaluate writes position/rotation/scale onto named entities
    // and FireMarkers writes clip/time/playing onto their Animators, which is why
    // both snapshot the scene and restore it when they end. Neither sets playMode_,
    // so a Ctrl+S mid-scrub used to write the previewed poses over the authored
    // level - and ending the preview then restored the in-memory scene, so nothing
    // on screen ever hinted that the file was wrong.
    {
        const fs::path file = dir / "Previewing.hbscene";
        Scene s;
        const entt::entity e = s.CreateEntity("Actor");
        s.Registry().emplace<Transform>(e);
        expect(scene::SaveScene(s, file), "author a scene to preview");
        for (int which = 0; which < 2; ++which) {
            Editor ed;
            ed.currentScenePath_ = file;
            (which == 0 ? ed.csPreview_ : ed.movieActive_) = true;
            const FileStamp before = Stamp(file);
            const bool wrote = ed.SaveSceneToDisk(s, file);
            const FileStamp after = Stamp(file);
            expect(!wrote, "SaveSceneToDisk refuses while a cutscene/movie preview poses "
                           "the scene");
            expect(Same(before, after), "...and writes nothing");
            expect(ed.saveStatusError_ &&
                       ed.saveStatus_.find("PREVIEW ACTIVE") != std::string::npos,
                   "...and says which preview it was");
        }
    }

    // --- 9c. A TYPE-MISMATCHED FIELD IS REFUSED, NOT A CRASH --------------------
    // Every field read goes through `it->value(key, default)`, which THROWS on a type
    // mismatch. ParseSceneJson used to sit outside the try/catch, so a `.hbscene`
    // whose `paint.layer` was a number (a hand edit, a half-migration, a foreign
    // tool) terminated the process with no message at all: a hard crash of an
    // unsaved editor session, and a crash on boot in the shipped runtime.
    //
    // NOT RUN IN A DEBUG BUILD, and the reason is a SEPARATE, PRE-EXISTING DEFECT,
    // not a weakness in this assertion: in the Debug configuration THIS PROGRAM
    // CANNOT SURVIVE A CAUGHT nlohmann EXCEPTION AT ALL. Throwing one trips
    // `_CrtIsValidHeapPointer(block)` in the debug heap and aborts, before the catch
    // body even runs. It is nothing to do with the scene serializer: it reproduces
    // through code this work never touched -
    //     HeartbreakEditor.exe --project <p> --test-uicanvas UI/Broken.hbui
    // on a `.hbui` containing malformed JSON aborts in Debug (0x80000003) and refuses
    // cleanly in Release, and that try/catch predates all of this. Running the
    // assertion here would report that pre-existing crash as a scene-save failure in
    // exactly one configuration while telling nobody what it actually was. The
    // contract IS verified in Release and RelWithDebInfo, which are the
    // configurations the editor and the shipped runtime are built in.
#if defined(_DEBUG)
    HBE_WARN("scenesave: SKIPPING the malformed-field section in a Debug build - this "
             "build aborts on any caught nlohmann exception (_CrtIsValidHeapPointer), "
             "a pre-existing defect reproducible with --test-uicanvas on a malformed "
             ".hbui. The section runs in Release / RelWithDebInfo.");
#else
    {
        const fs::path bad = dir / "BadTypes.hbscene";
        {
            std::ofstream f(bad);
            f << R"({"version":1,"entities":[{"name":"Broken","transform":{"p":[0,0,0]},)"
                 R"("paint":{"source":"Paint/x.hbpaint","layer":7}}]})";
        }
        scene::SceneData d;
        expect(!scene::ParseSceneFile(bad, d),
               "a type-mismatched field REFUSES the file (and does not terminate)");
        expect(d.entities.empty(), "...and leaves no half-parsed world behind");
        scene::SceneData d2;
        expect(!scene::ParseSceneString(R"({"entities":[{"paint":{"layer":7}}]})", d2),
               "the string parser refuses it too (undo/redo and Play snapshots)");
    }
#endif

    // --- 10. THE FILE WE WERE POINTED AT IS UNTOUCHED ---------------------------
    {
        const FileStamp originalAfter = Stamp(sceneFile);
        expect(Same(originalBefore, originalAfter),
               "the scene this test was pointed at was never written to");
    }

    // ON FAILURE THE ARTIFACTS STAY. B, C, the 0-tick and 600-tick saves and the
    // hazard round trips are what a diff needs; deleting them leaves whoever has to
    // fix this with a boolean.
    if (ok) fs::remove_all(dir, ec);
    else HBE_ERROR("scenesave: artifacts kept for diffing in '{}'.", dir.string());
    if (ok) {
        HBE_INFO("scenesave: '{}' - {} entities round-trip with no entity and no component "
                 "lost against the FILE ON DISK; the save is byte-identical after 0 and 600 "
                 "edit-mode ticks and is a fixed point; despawned shards, a replaced world, "
                 "an empty world over a populated file and Play mode are each REFUSED with "
                 "the target file untouched.",
                 sceneFile.filename().string(), censusA.byGuid.size());
    }
    return ok;
}

bool Editor::MaterialVolumeSaveSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("matvolume: FAILED - {}", what);
        }
    };
    const auto nearf = [](f32 a, f32 b) { return (a - b) < 1e-4f && (b - a) < 1e-4f; };

    // Author one entity with a fully NON-default MaterialVolumeComponent, plus a bare entity that
    // must not gain one; snapshot to a JSON string and parse it back (the Ctrl+S / collab path).
    Scene s;
    auto& reg = s.Registry();
    const entt::entity ve = s.CreateEntity("Vol");
    reg.emplace<Transform>(ve);
    MaterialVolumeComponent mv;
    mv.halfExtents = {3.0f, 1.5f, 4.25f};
    mv.falloffType = 4; // Ease In
    mv.falloffGamma = 2.5f;
    mv.falloffWidth = 0.4f;
    mv.strength = 0.8f;
    mv.material = "materials/brick.hbmat";
    mv.color = {0.2f, 0.4f, 0.6f, 0.9f};
    mv.metallic = 0.7f;
    mv.roughness = 0.33f;
    mv.projection = 2; // Triplanar
    mv.tileMeters = {0.5f, 1.25f, 2.0f};
    mv.blend = 1; // Height
    mv.opacity = 0.6f;
    mv.enabled = false;
    mv.bakeResolution = 2048;
    reg.emplace<MaterialVolumeComponent>(ve, mv);

    const entt::entity bare = s.CreateEntity("Bare");
    reg.emplace<Transform>(bare);

    const std::string text = scene::SaveSceneToString(s);
    expect(!text.empty(), "SaveSceneToString produced a non-empty snapshot");

    scene::SceneData d;
    expect(scene::ParseSceneString(text, d), "ParseSceneString parses the snapshot");

    const scene::EntityData* vd = nullptr;
    const scene::EntityData* bd = nullptr;
    for (const auto& e : d.entities) {
        if (e.name == "Vol") vd = &e;
        if (e.name == "Bare") bd = &e;
    }
    expect(vd != nullptr, "the volume entity survived the round trip");
    expect(bd != nullptr, "the bare entity survived the round trip");
    expect(bd && !bd->hasMaterialVolume, "the bare entity did NOT gain a material volume");

    if (vd) {
        expect(vd->hasMaterialVolume, "the volume entity kept its MaterialVolumeComponent");
        const MaterialVolumeComponent& g = vd->materialVolume;
        expect(nearf(g.halfExtents.x, 3.0f) && nearf(g.halfExtents.y, 1.5f) &&
                   nearf(g.halfExtents.z, 4.25f),
               "halfExtents round-trips");
        expect(g.falloffType == 4, "falloffType round-trips");
        expect(nearf(g.falloffGamma, 2.5f), "falloffGamma round-trips");
        expect(nearf(g.falloffWidth, 0.4f), "falloffWidth round-trips");
        expect(nearf(g.strength, 0.8f), "strength round-trips");
        expect(g.material == "materials/brick.hbmat", "material ref round-trips");
        expect(nearf(g.color.r, 0.2f) && nearf(g.color.g, 0.4f) && nearf(g.color.b, 0.6f) &&
                   nearf(g.color.a, 0.9f),
               "color round-trips");
        expect(nearf(g.metallic, 0.7f), "metallic round-trips");
        expect(nearf(g.roughness, 0.33f), "roughness round-trips");
        expect(g.projection == 2, "projection round-trips");
        expect(nearf(g.tileMeters.x, 0.5f) && nearf(g.tileMeters.y, 1.25f) &&
                   nearf(g.tileMeters.z, 2.0f),
               "tileMeters round-trips");
        expect(g.blend == 1, "blend round-trips");
        expect(nearf(g.opacity, 0.6f), "opacity round-trips");
        expect(!g.enabled, "enabled=false round-trips");
        expect(g.bakeResolution == 2048, "bakeResolution round-trips");
    }

    if (ok)
        HBE_INFO("matvolume: MaterialVolumeComponent round-trips through the scene serializer "
                 "with every field intact.");
    return ok;
}

} // namespace hbe
