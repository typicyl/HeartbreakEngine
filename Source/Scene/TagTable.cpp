// Scene/TagTable.cpp - the streaming tag table (see TagTable.h for the rules).
#include "Scene/TagTable.h"

#include "Core/Log.h"
#include "Scene/Scene.h"
#include "Scene/Hierarchy.h" // scene::SubtreeInOrder (one parent->children pass)
#include "Scene/SceneSerializer.h"
#include "Scene/StreamingSalvage.h" // SALVAGE 2 - the hysteresis clamp

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace hbe::tags {

namespace {

// Highest id a TagId can express. The table refuses to grow past it rather than
// wrapping a u16 back onto Untagged.
constexpr usize kMaxTags = 65535;

struct Table {
    std::vector<std::string> names;
    std::unordered_map<std::string, TagId> byName;

    Table() { Clear(); }
    void Clear() {
        names.assign(1, std::string(kUntaggedName));
        byName.clear();
        byName.emplace(std::string(kUntaggedName), kTagUntagged);
    }
};

Table& T() {
    static Table t;
    return t;
}

const std::string& Empty() {
    static const std::string s;
    return s;
}

} // namespace

TagId Intern(const std::string& name) {
    if (name.empty()) return kTagUntagged; // "" means untagged, not a new tag
    Table& t = T();
    if (const auto it = t.byName.find(name); it != t.byName.end()) return it->second;
    if (t.names.size() > kMaxTags) {
        HBE_WARN("tags: table full ({} tags); '{}' falls back to Untagged.", kMaxTags, name);
        return kTagUntagged;
    }
    const TagId id = static_cast<TagId>(t.names.size());
    t.names.push_back(name);
    t.byName.emplace(name, id);
    return id;
}

const std::string& Name(TagId id) {
    const Table& t = T();
    if (static_cast<usize>(id) < t.names.size()) return t.names[static_cast<usize>(id)];
    return Empty();
}

TagId Find(const std::string& name) {
    const Table& t = T();
    const auto it = t.byName.find(name);
    return it != t.byName.end() ? it->second : kTagUntagged;
}

void Reset() { T().Clear(); }

void SeedFromProject(const std::vector<TagDef>& defs) {
    Reset();
    // Normalize guarantees defs[0] is "Untagged" and that names are unique, so
    // interning in order reproduces index == TagId exactly. Seeding an
    // un-normalized list would still be safe (a duplicate simply re-returns the
    // earlier id) but the indices would stop matching, which is why the caller
    // normalizes first.
    for (const TagDef& d : defs) Intern(d.name);
}

const std::vector<std::string>& All() { return T().names; }

void Normalize(std::vector<TagDef>& defs) {
    std::vector<TagDef> outv;
    outv.reserve(defs.size() + 1);

    // Index 0 is "Untagged", always. An authored row of that name is adopted for
    // its other fields (someone may have edited its priority) but it cannot move
    // and it cannot be un-pinned: alwaysLoaded is forced back on.
    TagDef untagged;
    untagged.name = kUntaggedName;
    for (const TagDef& d : defs)
        if (d.name == kUntaggedName) {
            untagged = d;
            break;
        }
    untagged.name = kUntaggedName;
    untagged.alwaysLoaded = true;
    outv.push_back(untagged);

    std::unordered_set<std::string> seen;
    seen.insert(std::string(kUntaggedName));
    for (const TagDef& d : defs) {
        if (d.name.empty()) continue; // skip-invalid-rows, as audioBuses/inputActions do
        if (!seen.insert(d.name).second) {
            // The "Untagged" row is expected to collide - it was hoisted to index 0
            // above, and re-reporting that on every parse would be pure noise.
            if (d.name != kUntaggedName) {
                HBE_WARN("tags: duplicate tag '{}' in the project list; the first row wins.",
                         d.name);
            }
            continue; // two rows of one name would collapse onto one TagId
        }
        outv.push_back(d);
    }

    for (TagDef& d : outv) {
        // RULE 6's authored list. ONLY the unambiguously meaningless is removed: an
        // empty name, a self-reference (a tag cannot pull itself in) and a repeat.
        // A name this project does not list is KEPT - see the header for why
        // dropping it here would eat authored data on a keystroke.
        if (!d.associates.empty()) {
            std::vector<std::string> keep;
            keep.reserve(d.associates.size());
            for (const std::string& a : d.associates) {
                if (a.empty() || a == d.name) continue;
                if (std::find(keep.begin(), keep.end(), a) != keep.end()) continue;
                keep.push_back(a);
            }
            d.associates = std::move(keep);
        }
        if (d.loadRadius < 0.0f) d.loadRadius = 0.0f;
        if (d.shardCell < 0.0f) d.shardCell = 0.0f;
        const f32 before = d.unloadRadius;
        salvage::EnforceHysteresis(d.loadRadius, d.unloadRadius);
        if (d.unloadRadius != before) {
            HBE_WARN("tags: '{}' unloadRadius {} <= loadRadius {}; corrected to {} "
                     "(a degenerate band thrashes spawn/despawn every frame).",
                     d.name, before, d.loadRadius, d.unloadRadius);
        }
    }
    defs = std::move(outv);
}

void ReconcileWithTable(std::vector<TagDef>& defs) {
    const std::vector<std::string>& names = All();
    std::vector<TagDef> outv;
    outv.reserve(names.size() + defs.size());
    // One row per interned tag, in ID ORDER, keeping the authored config when the
    // list already had that name.
    for (const std::string& n : names) {
        TagDef row;
        for (const TagDef& d : defs)
            if (d.name == n) {
                row = d;
                break;
            }
        row.name = n;
        outv.push_back(std::move(row));
    }
    // Listed-but-not-interned rows (a tag added to the `.hbproj` by hand after the
    // table was seeded) are interned now, which lands them at exactly outv.size().
    for (const TagDef& d : defs) {
        if (d.name.empty()) continue;
        bool have = false;
        for (const TagDef& o : outv)
            if (o.name == d.name) {
                have = true;
                break;
            }
        if (have) continue;
        Intern(d.name);
        outv.push_back(d);
    }
    Normalize(outv);
    defs = std::move(outv);
}

void BuildAssocGraph(const std::vector<TagDef>& defs, stream::AssocGraph& out,
                     std::vector<std::string>* unresolvedOut) {
    out.edges.assign(defs.size(), {});
    if (unresolvedOut) unresolvedOut->clear();
    if (defs.empty()) return;
    // One map, then a lookup per edge: O(tags + edges), not O(tags x edges). Built
    // once per bind, never per evaluation.
    std::unordered_map<std::string, u32> byName;
    byName.reserve(defs.size());
    for (usize i = 0; i < defs.size(); ++i) byName.emplace(defs[i].name, static_cast<u32>(i));
    for (usize i = 0; i < defs.size(); ++i) {
        for (const std::string& a : defs[i].associates) {
            const auto it = byName.find(a);
            if (it == byName.end()) {
                // Not a tag this project lists. The edge cannot become an index, so
                // it is dropped HERE and reported ONCE by the caller; the authored
                // string itself is untouched (a hand-edited `.hbproj` and a target
                // that is about to be added both survive a round trip).
                if (unresolvedOut) unresolvedOut->push_back(defs[i].name + " -> " + a);
                continue;
            }
            if (it->second == static_cast<u32>(i)) continue; // Normalize drops these
            out.edges[i].push_back(it->second);
        }
    }
}

bool RemoveTag(entt::registry& reg, std::vector<TagDef>& defs, usize index) {
    if (index == 0) {
        HBE_WARN("tags: '{}' is undeletable (it is what an untagged entity means).",
                 kUntaggedName);
        return false;
    }
    if (index >= defs.size()) return false;
    // Adopt any auto-interned tag first, so the re-seed at the end cannot drop it
    // and orphan the entities holding its id. Reconcile only APPENDS, so `index`
    // still points at the row the caller meant.
    ReconcileWithTable(defs);
    if (index >= defs.size()) return false;

    // Remap BEFORE the row goes away, so ids still line up with the new order.
    // An entity on the removed tag becomes untagged (component dropped); an
    // entity above it shifts down by one. Skipping this would repoint every
    // entity above `index` at its neighbour's tag.
    const TagId gone = static_cast<TagId>(index);
    std::vector<entt::entity> untag;
    for (const entt::entity e : reg.view<Tag>()) {
        if (!reg.valid(e)) continue;
        Tag& t = reg.get<Tag>(e);
        if (t.id == gone) untag.push_back(e);
        else if (t.id > gone) t.id = static_cast<TagId>(t.id - 1);
    }
    for (const entt::entity e : untag) reg.remove<Tag>(e);

    // The dead tag's NAME leaves every other row's association list with it. A name
    // is not an index, so this is an erase and not a third remap - which is the
    // whole reason RULE 6 stores names. Done before the row goes, so `gone`'s name
    // is still readable.
    const std::string goneName = defs[index].name;
    defs.erase(defs.begin() + static_cast<std::ptrdiff_t>(index));
    for (TagDef& d : defs)
        d.associates.erase(std::remove(d.associates.begin(), d.associates.end(), goneName),
                           d.associates.end());
    Normalize(defs);
    SeedFromProject(defs);
    return true;
}

bool Taggable(const entt::registry& reg, entt::entity e) {
    if (!reg.valid(e)) return false;
    return !reg.all_of<UIDocMember>(e); // UI documents are outside the streamed world
}

bool Assign(entt::registry& reg, entt::entity e, TagId id, i32 shard) {
    if (!Taggable(reg, e)) return false;
    if (id == kTagUntagged) {
        reg.remove<Tag>(e); // absence IS Untagged; never store id 0
        return true;
    }
    reg.emplace_or_replace<Tag>(e, Tag{id, shard});
    return true;
}

usize AssignSubtree(entt::registry& reg, entt::entity root, TagId id, i32 shard) {
    if (!reg.valid(root)) return 0;
    usize n = 0;
    // ONE parent->children pass (Scene/Hierarchy.h) instead of a full view<Parent>
    // scan per visited node - this was O(subtree x world), on a path an author hits
    // from the Inspector.
    for (const entt::entity cur : scene::SubtreeInOrder(reg, root))
        if (Assign(reg, cur, id, shard)) ++n;
    return n;
}

// --- --test-tagtable ----------------------------------------------------------

bool SelfTest() {
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("tagtable: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_tagtable";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 1) INTERNING ROUND-TRIPS, and Untagged is index 0.
    {
        Reset();
        expect(All().size() == 1, "a reset table holds exactly one tag");
        expect(Name(kTagUntagged) == kUntaggedName, "index 0 is \"Untagged\"");
        expect(Find(kUntaggedName) == kTagUntagged, "\"Untagged\" resolves to id 0");
        expect(Intern(kUntaggedName) == kTagUntagged,
               "interning \"Untagged\" must not create a second row");

        const TagId camp = Intern("Camp");
        const TagId mill = Intern("Interior_Mill");
        expect(camp == 1 && mill == 2, "ids are assigned in intern order from 1");
        expect(Intern("Camp") == camp, "interning an existing name returns the same id");
        expect(Name(camp) == "Camp" && Name(mill) == "Interior_Mill",
               "id -> name round-trips");
        expect(Find("Camp") == camp, "name -> id round-trips");
        expect(Find("NoSuchTag") == kTagUntagged, "an unknown name resolves to Untagged");
        expect(Name(static_cast<TagId>(9999)).empty(), "an unknown id has no name");
        expect(Intern("") == kTagUntagged, "an empty name is Untagged, not a new tag");
        expect(All().size() == 3, "the table grew by exactly the two new tags");

        Reset();
        expect(All().size() == 1 && Find("Camp") == kTagUntagged,
               "Reset drops every project-specific tag (the project-switch bug)");
    }

    // 2) Normalize: Untagged is forced to index 0 + alwaysLoaded and is
    //    UNDELETABLE; bad radii are corrected; junk rows are dropped.
    {
        std::vector<TagDef> defs;
        defs.push_back({"Camp", 100.0f, 50.0f, 0, false, true, 0.0f}); // unload <= load
        defs.push_back({"", 10.0f, 20.0f, 0, false, true, 0.0f});      // nameless
        defs.push_back({"Camp", 999.0f, 9999.0f, 0, false, true, 0.0f}); // duplicate
        defs.push_back({kUntaggedName, 1.0f, 2.0f, 7, false, true, 0.0f}); // not first, unpinned
        defs.push_back({"Zeroed", 0.0f, 0.0f, 0, false, true, 0.0f});    // degenerate band
        Normalize(defs);

        expect(defs.size() == 3, "Normalize drops the nameless and duplicate rows");
        expect(defs[0].name == kUntaggedName, "Normalize moves \"Untagged\" to index 0");
        expect(defs[0].alwaysLoaded, "\"Untagged\" is alwaysLoaded even when authored off");
        expect(defs[0].priority == 7, "an authored \"Untagged\" row keeps its other fields");
        expect(defs[1].name == "Camp" && defs[2].name == "Zeroed",
               "the surviving rows keep their authored order");
        expect(defs[1].loadRadius == 100.0f && defs[1].unloadRadius == 126.0f,
               "unload <= load is corrected to load * 1.25 + 1 (hysteresis)");
        expect(defs[2].unloadRadius == 1.0f,
               "a zeroed band still separates (the + 1 in the clamp)");
        expect(defs[1].unloadRadius > defs[1].loadRadius && defs[2].unloadRadius > 0.0f,
               "every normalized tag has a non-degenerate hysteresis band");

        SeedFromProject(defs);
        expect(All().size() == 3 && Find("Camp") == 1 && Find("Zeroed") == 2,
               "SeedFromProject makes TagId == index into the authored list");
    }

    // 3) The `.hbproj` round trip, plus the two parse rules the inputActions
    //    precedent records: a REPEATED parse must replace rather than accumulate,
    //    and present-but-empty must not resurrect anything.
    const fs::path projDir = dir / "P";
    {
        expect(Project::Active().Create(projDir, "P"), "create a scratch project");
        ProjectSettings& s = Project::Active().Settings();
        expect(s.tags.size() == 1 && s.tags[0].name == kUntaggedName,
               "a fresh project starts with just \"Untagged\"");
        s.tags.push_back({"Camp", 90.0f, 130.0f, 3, false, true, 40.0f});
        s.tags.push_back({"Interior_Mill", 25.0f, 5.0f, -1, true, false, 0.0f});
        Normalize(s.tags);
        expect(Project::Active().Save(), "save the project with tags");

        const fs::path hbproj = projDir / "P.hbproj";
        expect(Project::Active().Open(hbproj), "reopen the project");
        const std::vector<TagDef>& t = Project::Active().Settings().tags;
        expect(t.size() == 3, "the tag list round-trips through the .hbproj");
        if (t.size() == 3) {
            expect(t[0].name == kUntaggedName && t[0].alwaysLoaded, "Untagged survives at 0");
            expect(t[1].name == "Camp" && t[1].loadRadius == 90.0f &&
                       t[1].unloadRadius == 130.0f && t[1].priority == 3 &&
                       !t[1].alwaysLoaded && t[1].autoShard && t[1].shardCell == 40.0f,
                   "every TagDef field round-trips");
            expect(t[2].name == "Interior_Mill" && t[2].alwaysLoaded && !t[2].autoShard,
                   "the flags round-trip independently");
            expect(t[2].unloadRadius == 25.0f * 1.25f + 1.0f,
                   "the hysteresis clamp is applied AT PARSE, not just on edit");
        }
        expect(Find("Camp") == 1 && Find("Interior_Mill") == 2,
               "opening a project reseeds the table from its list");

        // Re-open the SAME project: the list must not double (the reused
        // settings_ member that inputActions clears for exactly this reason).
        expect(Project::Active().Open(hbproj), "reopen the project a second time");
        expect(Project::Active().Settings().tags.size() == 3,
               "a repeated parse REPLACES the tag list rather than appending");

        // Present-but-empty and absent both mean "only Untagged" - the list can
        // never be legitimately empty, unlike inputActions.
        const auto parseRaw = [&](const char* body) {
            const fs::path p = projDir / "Raw.hbproj";
            std::ofstream f(p);
            f << body;
            f.close();
            expect(Project::Active().Open(p), "open a hand-written .hbproj");
            return Project::Active().Settings().tags;
        };
        {
            const std::vector<TagDef> t2 = parseRaw("{\"name\":\"R\",\"tags\":[]}");
            expect(t2.size() == 1 && t2[0].name == kUntaggedName,
                   "a present-but-empty tags array yields exactly \"Untagged\"");
        }
        {
            const std::vector<TagDef> t2 = parseRaw("{\"name\":\"R\"}");
            expect(t2.size() == 1 && t2[0].name == kUntaggedName,
                   "an absent tags key yields exactly \"Untagged\"");
        }
        // Back to the real list for the scene tests below.
        expect(Project::Active().Open(hbproj), "restore the tagged project");
    }

    // 4) A per-entity tag survives save -> parse -> save, and does so
    //    BYTE-IDENTICALLY over two round trips (the LevelTypesSelfTest
    //    discipline: one cycle reverses the entity array, two return to the
    //    original bytes).
    const TagId camp = Find("Camp");
    const TagId mill = Find("Interior_Mill");
    expect(camp != kTagUntagged && mill != kTagUntagged, "the scratch tags are interned");
    {
        Scene m;
        auto& reg = m.Registry();
        const entt::entity root = m.CreateEntity("CampRoot");
        reg.emplace<Transform>(root);
        const entt::entity child = m.CreateEntity("Tent");
        reg.emplace<Transform>(child);
        reg.emplace<Parent>(child, Parent{root});
        const entt::entity loose = m.CreateEntity("Rock");
        reg.emplace<Transform>(loose);
        reg.emplace<SceneLayer>(loose, SceneLayer{SceneKind::Static});

        // 4a) Subtree assignment: one click tags the root AND its descendants.
        expect(AssignSubtree(reg, root, camp) == 2,
               "AssignSubtree tags the root and every descendant");
        expect(reg.all_of<Tag>(root) && reg.all_of<Tag>(child),
               "the whole subtree carries the tag");
        expect(reg.get<Tag>(child).id == camp, "the descendant got the SAME tag");
        expect(!reg.all_of<Tag>(loose), "an unrelated entity is untouched");

        // 4b) Assigning Untagged REMOVES the component (absence is the id).
        expect(Assign(reg, loose, mill) && reg.all_of<Tag>(loose), "tag a loose entity");
        expect(Assign(reg, loose, kTagUntagged) && !reg.all_of<Tag>(loose),
               "assigning Untagged removes the component rather than storing id 0");
        expect(Assign(reg, loose, mill, 4) && reg.get<Tag>(loose).shard == 4,
               "the baked shard index is stored alongside the id");

        const fs::path r0 = dir / "Round0.hbscene";
        expect(scene::SaveScene(m, r0), "save a tagged scene");

        scene::SceneData d0;
        expect(scene::ParseSceneFile(r0, d0), "the tagged scene parses");
        expect(d0.entities.size() == 3, "all three entities round-trip");
        int tagged = 0, sharded = 0, untagged = 0;
        for (const scene::EntityData& e : d0.entities) {
            if (!e.hasTag) { ++untagged; expect(e.shard == -1, "an untagged row has shard -1"); }
            else {
                ++tagged;
                expect(e.tag == "Camp" || e.tag == "Interior_Mill",
                       "the tag NAME (not the id) is what serializes");
                if (e.shard >= 0) ++sharded;
            }
        }
        expect(tagged == 3 && untagged == 0, "every tagged entity kept its tag");
        expect(sharded == 1, "\"shard\" is written only when it was baked");

        // The raw JSON: names, not ids, and no "shard" key on an unbaked entity.
        {
            std::ifstream f(r0, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            expect(text.find("\"tag\": \"Camp\"") != std::string::npos,
                   "the file stores the tag as a NAME");
            expect(text.find("\"shard\": 4") != std::string::npos,
                   "the file stores the baked shard index");
            usize shards = 0;
            for (usize at = text.find("\"shard\""); at != std::string::npos;
                 at = text.find("\"shard\"", at + 1))
                ++shards;
            expect(shards == 1, "\"shard\" is absent on unbaked entities, not written as -1");
        }

        // Two round trips must return to the first save's bytes.
        const auto rebuild = [](const scene::SceneData& src, Scene& dst) {
            std::vector<entt::entity> made;
            for (const scene::EntityData& e : src.entities) {
                const entt::entity ne = dst.CreateEntity(e.name);
                if (e.guid != 0) dst.Registry().emplace_or_replace<Guid>(ne, Guid{e.guid});
                if (e.hasTransform) dst.Registry().emplace<Transform>(ne, e.transform);
                if (e.hasSceneLayerTag)
                    dst.Registry().emplace<SceneLayer>(ne, SceneLayer{e.sceneLayerKind});
                if (e.hasTag) Assign(dst.Registry(), ne, Intern(e.tag), e.shard);
                made.push_back(ne);
            }
            for (usize i = 0; i < src.entities.size(); ++i) {
                const int p = src.entities[i].parent;
                if (p >= 0 && p < static_cast<int>(made.size()))
                    dst.Registry().emplace<Parent>(made[i],
                                                   Parent{made[static_cast<usize>(p)]});
            }
        };
        const auto readAll = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        };
        const fs::path r1 = dir / "Round1.hbscene";
        const fs::path r2 = dir / "Round2.hbscene";
        Scene s1;
        rebuild(d0, s1);
        expect(scene::SaveScene(s1, r1), "re-save the rebuilt tagged scene");
        scene::SceneData d1;
        expect(scene::ParseSceneFile(r1, d1), "the re-saved scene parses");
        Scene s2;
        rebuild(d1, s2);
        expect(scene::SaveScene(s2, r2), "re-save after a second round trip");
        const std::string t0 = readAll(r0), t2 = readAll(r2);
        expect(!t0.empty() && t0 == t2,
               "two round trips of a TAGGED scene are byte-identical to the first save");

        // 4c) A clipboard/prefab fragment keeps the tag (a clone belongs to the
        //     same streaming group) but drops the BAKED shard and the guid.
        //     Bake a shard onto the subtree FIRST so the drop is not vacuous
        //     (done after the round-trip files above, which must stay as saved).
        {
            expect(AssignSubtree(reg, root, camp, 7) == 2, "bake a shard onto the subtree");
            const std::string frag = scene::SaveSubtreeToString(m, root);
            expect(frag.find("\"tag\"") != std::string::npos,
                   "a copied subtree keeps its tag");
            expect(frag.find("\"shard\"") == std::string::npos,
                   "a copied subtree drops the baked shard (it is re-baked on save)");
            expect(frag.find("\"guid\"") == std::string::npos,
                   "a copied subtree still drops identity");
            scene::SceneData fd;
            expect(scene::ParseSceneString(frag, fd), "the fragment parses");
            expect(fd.entities.size() == 2, "the fragment is the whole subtree");
            for (const scene::EntityData& e : fd.entities) {
                expect(e.hasTag && e.tag == "Camp", "every fragment row kept the tag");
                expect(e.shard == -1, "every fragment row is unbaked");
            }
        }

        // 4d) An unknown tag name AUTO-INTERNS - it never silently becomes
        //     Untagged, which would move content into the always-resident set.
        {
            const usize before = All().size();
            const TagId ghost = Intern("TagNotInTheProject");
            expect(ghost != kTagUntagged && All().size() == before + 1,
                   "a scene-only tag auto-interns instead of folding into Untagged");
        }
    }

    // 5) RemoveTag: index 0 is undeletable, and removing a row REMAPS live
    //    entities instead of repointing them at their neighbour's tag.
    {
        std::vector<TagDef> defs;
        defs.push_back({"A", 10.0f, 20.0f, 0, false, true, 0.0f});
        defs.push_back({"B", 10.0f, 20.0f, 0, false, true, 0.0f});
        defs.push_back({"C", 10.0f, 20.0f, 0, false, true, 0.0f});
        Normalize(defs);
        SeedFromProject(defs);
        const TagId a = Find("A"), b = Find("B"), c = Find("C");
        expect(a == 1 && b == 2 && c == 3, "A/B/C seed as ids 1/2/3");

        Scene t;
        auto& reg = t.Registry();
        const entt::entity ea = t.CreateEntity("onA");
        const entt::entity eb = t.CreateEntity("onB");
        const entt::entity ecc = t.CreateEntity("onC");
        Assign(reg, ea, a);
        Assign(reg, eb, b, 2);
        Assign(reg, ecc, c);

        expect(!RemoveTag(reg, defs, 0), "\"Untagged\" is undeletable");
        expect(defs.size() == 4 && defs[0].name == kUntaggedName,
               "a refused delete changes nothing");
        expect(!RemoveTag(reg, defs, 99), "an out-of-range delete is refused");

        expect(RemoveTag(reg, defs, static_cast<usize>(b)), "delete tag B");
        expect(defs.size() == 3 && defs[1].name == "A" && defs[2].name == "C",
               "the row is gone and the rest keep their order");
        expect(!reg.all_of<Tag>(eb), "an entity on the deleted tag becomes untagged");
        expect(reg.all_of<Tag>(ea) && reg.get<Tag>(ea).id == Find("A"),
               "an entity BELOW the deleted row keeps its tag");
        expect(reg.all_of<Tag>(ecc) && reg.get<Tag>(ecc).id == Find("C"),
               "an entity ABOVE the deleted row was remapped, not repointed at B");
        expect(Name(reg.get<Tag>(ecc).id) == "C",
               "the remapped id still names the SAME tag");
    }

    // 5b) ReconcileWithTable: an AUTO-INTERNED tag (one a scene referenced but the
    //     project never listed) must survive a list edit. Without the reconcile,
    //     re-seeding from the shorter list drops it, and the entity holding its id
    //     has no name - so the next save writes no "tag" and the object silently
    //     leaves its streaming group.
    {
        std::vector<TagDef> defs;
        defs.push_back({"Listed", 10.0f, 20.0f, 0, false, true, 0.0f});
        Normalize(defs);
        SeedFromProject(defs);
        const TagId listed = Find("Listed");
        const TagId ghost = Intern("FromASceneOnly"); // auto-interned, unlisted
        expect(listed == 1 && ghost == 2, "the auto-interned tag lands after the listed one");
        expect(defs.size() == 2 && All().size() == 3,
               "the table is AHEAD of the authored list, which is the hazard");

        Scene t;
        auto& reg = t.Registry();
        const entt::entity eg = t.CreateEntity("onGhost");
        Assign(reg, eg, ghost);

        ReconcileWithTable(defs);
        expect(defs.size() == 3 && defs[2].name == "FromASceneOnly",
               "Reconcile adopts the auto-interned tag into the authored list");
        expect(Find("FromASceneOnly") == ghost && Find("Listed") == listed,
               "Reconcile appends only - no existing id moves");
        expect(defs[2].loadRadius == 120.0f && defs[2].unloadRadius == 160.0f,
               "an adopted tag gets the default streaming config");

        // Now a delete elsewhere in the list cannot orphan it.
        expect(RemoveTag(reg, defs, static_cast<usize>(listed)), "delete the listed tag");
        expect(reg.all_of<Tag>(eg) && Name(reg.get<Tag>(eg).id) == "FromASceneOnly",
               "the adopted tag's entity still names the SAME tag after a delete");

        // A row added to the `.hbproj` by hand after seeding is interned by the
        // reconcile rather than being silently unusable.
        defs.push_back({"HandEdited", 10.0f, 20.0f, 0, false, true, 0.0f});
        ReconcileWithTable(defs);
        expect(Find("HandEdited") != kTagUntagged, "Reconcile interns a listed-but-unknown row");
        expect(All()[static_cast<usize>(Find("HandEdited"))] == "HandEdited" &&
                   defs[static_cast<usize>(Find("HandEdited"))].name == "HandEdited",
               "after Reconcile, defs[i].name == All()[i] for every tag");
    }

    // 6) A `.hbui` DOCUMENT entity can never be tagged - enforced, not
    //    discouraged. UI is asset content: it is excluded from every scene write
    //    and spared by every Replace sweep, so a streaming group containing it
    //    could never spawn or despawn.
    {
        // Section 5 reseeded the table, so re-intern here rather than reusing an
        // id whose NAME has since changed underneath it.
        const TagId campId = Intern("Camp");
        Scene t;
        auto& reg = t.Registry();
        const entt::entity widget = t.CreateEntity("Button");
        reg.emplace<Transform>(widget);
        reg.emplace<UIElement>(widget);
        reg.emplace<UIDocMember>(widget, UIDocMember{1u, true});

        expect(!Taggable(reg, widget), "a document entity is not taggable");
        expect(!Assign(reg, widget, campId), "Assign REFUSES a document entity");
        expect(!reg.all_of<Tag>(widget), "the refusal wrote nothing");

        // Through a subtree assignment too: a world root whose child was adopted
        // into a document tags the root only.
        const entt::entity root = t.CreateEntity("Root");
        reg.emplace<Transform>(root);
        reg.emplace<Parent>(widget, Parent{root});
        expect(AssignSubtree(reg, root, campId) == 1,
               "AssignSubtree skips the document member inside the subtree");
        expect(reg.all_of<Tag>(root) && !reg.all_of<Tag>(widget),
               "only the world half of the subtree is tagged");

        expect(Taggable(reg, entt::null) == false, "an invalid handle is not taggable");

        // Belt and braces: even a Tag forced on by hand (a hand-edited file)
        // cannot reach a `.hbscene`, because document members are not written.
        reg.emplace<Tag>(widget, Tag{campId, 0});
        const fs::path p = dir / "DocLeak.hbscene";
        expect(scene::SaveScene(t, p), "save a scene alongside a document");
        std::ifstream f(p, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        expect(text.find("\"Button\"") == std::string::npos,
               "a document entity is not written into the scene at all");
        expect(text.find("\"tag\": \"Camp\"") != std::string::npos,
               "the world root's tag WAS written (the check above is not vacuous)");
    }

    fs::remove_all(dir, ec);
    Reset();
    if (ok) {
        HBE_INFO("tagtable: interning, Untagged-at-0, the hysteresis clamp, the "
                 ".hbproj round trip, byte-identical scene round trips, subtree "
                 "assignment, delete-with-remap and the UI-document refusal all hold.");
    }
    return ok;
}

} // namespace hbe::tags
