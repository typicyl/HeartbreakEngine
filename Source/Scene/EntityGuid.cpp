// Scene/EntityGuid.cpp - see EntityGuid.h for the design and the four rules.
#include "Scene/EntityGuid.h"

#include "Core/Log.h"
#include "Project/Project.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace hbe::guid {

namespace {

// SplitMix64 finalizer: the standard cheap 64-bit avalanche. Used for both the
// path hash's final mix and the index derivation, so a one-bit change in either
// the file identity or the entity index scatters the whole result.
u64 Mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

u64 Fnv1a(std::string_view s) {
    u64 h = 0xCBF29CE484222325ull;
    for (const char c : s) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

// The file identity a derived guid is anchored to. Relative to the project's
// Assets dir when the path is under it (so a moved/copied project keeps its
// guids), else the filename. Lowercased with forward slashes: the same scene
// reaches ParseSceneFile from the editor, the VFS and the packer with different
// casing and separators, and all three must derive the same guids.
std::string NormalizePathKey(const std::filesystem::path& path) {
    std::filesystem::path rel = path;
    if (Project::HasActive()) {
        std::error_code ec;
        const std::filesystem::path r =
            std::filesystem::relative(path, Project::Active().AssetsDir(), ec);
        if (!ec && !r.empty() && r.native().rfind(L"..", 0) != 0) rel = r;
    }
    std::string s = rel.generic_string();
    if (s.rfind("..", 0) == 0 || rel.is_absolute()) s = path.filename().generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

u64 Mint() {
    // Per-thread PRNG (StageAssets/streaming run on workers; CreateEntity is
    // main-thread today, but a minter that quietly breaks if that changes is a
    // trap) plus a process-wide counter so two threads seeded in the same tick
    // cannot walk the same sequence.
    static std::atomic<u64> counter{0};
    thread_local std::mt19937_64 rng{
        static_cast<u64>(std::random_device{}()) ^
        (static_cast<u64>(std::random_device{}()) << 32) ^
        static_cast<u64>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    u64 g = 0;
    while (g == 0) // 0 is reserved for "unset"
        g = rng() ^ Mix(counter.fetch_add(1, std::memory_order_relaxed));
    return g;
}

u64 SeedFromPath(const std::filesystem::path& path) {
    return Mix(Fnv1a(NormalizePathKey(path)) ^ 0x5BF03635A1D2E7C9ull);
}

u64 Derive(u64 seed, u32 index) {
    const u64 g = Mix(seed ^ Mix(static_cast<u64>(index) + 0x9E3779B9ull));
    return g ? g : 1ull; // 0 is reserved
}

std::string ToHex(u64 g) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(g));
    return std::string(buf, 16);
}

u64 FromHex(std::string_view s) {
    if (s.empty() || s.size() > 16) return 0;
    u64 v = 0;
    for (const char c : s) {
        u64 d;
        if (c >= '0' && c <= '9') d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u64>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') d = static_cast<u64>(c - 'A') + 10;
        else return 0; // malformed -> unset, so the loader mints instead
        v = (v << 4) | d;
    }
    return v;
}

Claim::Claim(const entt::registry& reg) {
    for (const entt::entity e : reg.view<const Guid>()) {
        const u64 g = reg.get<const Guid>(e).value;
        if (g) used_.insert(g);
    }
}

u64 Claim::Resolve(u64 parsed) {
    if (parsed != 0 && used_.insert(parsed).second) return parsed; // free -> adopt
    u64 g = Mint();
    while (!used_.insert(g).second) g = Mint(); // (never expected to loop)
    return g;
}

void Apply(entt::registry& reg, entt::entity e, u64 parsed, Claim& claim) {
    reg.emplace_or_replace<Guid>(e, Guid{claim.Resolve(parsed)});
}

// -----------------------------------------------------------------------------
// --test-entityguid
// -----------------------------------------------------------------------------
// Headless (no GPU, no window), so it cannot call scene::Instantiate - that
// needs a live Renderer. It drives the exact functions Instantiate drives
// instead (guid::Claim + guid::Apply, and the real BuildSceneJson /
// BuildSubtreeJson / ParseSceneString / ParseSceneFile writers and parsers), so
// the only thing not covered is the two-line call site in Instantiate itself.
namespace {

struct Check {
    bool ok = true;
    void Expect(bool cond, const char* what) {
        if (cond) return;
        ok = false;
        std::printf("  FAIL: %s\n", what);
    }
};

// Exactly what scene::Instantiate's create loop does for guids, so the test
// measures the shipping behaviour rather than a re-implementation of it.
std::vector<u64> FakeInstantiate(Scene& dst, const scene::SceneData& data) {
    auto& reg = dst.Registry();
    Claim claim(reg);
    std::vector<u64> out;
    out.reserve(data.entities.size());
    for (const scene::EntityData& d : data.entities) {
        const entt::entity e = dst.CreateEntity(d.name); // mints (rule 1)
        Apply(reg, e, d.guid, claim);                    // adopts or re-mints (rules 2+3)
        if (d.hasTransform) reg.emplace<Transform>(e, d.transform);
        out.push_back(reg.get<Guid>(e).value);
    }
    return out;
}

std::vector<u64> LiveGuids(const Scene& s) {
    std::vector<u64> out;
    for (const entt::entity e : s.Registry().view<const Guid>())
        out.push_back(s.Registry().get<const Guid>(e).value);
    return out;
}

bool AllUniqueNonZero(const std::vector<u64>& v) {
    std::unordered_set<u64> seen;
    for (const u64 g : v)
        if (g == 0 || !seen.insert(g).second) return false;
    return true;
}

bool Disjoint(const std::vector<u64>& a, const std::vector<u64>& b) {
    const std::unordered_set<u64> s(a.begin(), a.end());
    for (const u64 g : b)
        if (s.count(g)) return false;
    return true;
}

} // namespace

bool SelfTest() {
    Check c;

    // --- 0. hex round-trip (the serialized representation) --------------------
    for (const u64 g : {1ull, 0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull,
                        0x8000000000000000ull}) {
        c.Expect(FromHex(ToHex(g)) == g, "hex round-trip");
    }
    c.Expect(ToHex(0x1ull).size() == 16, "hex is zero-padded to 16 chars");
    c.Expect(FromHex("nothex") == 0, "malformed hex parses as unset");
    c.Expect(FromHex("") == 0, "empty hex parses as unset");

    // --- 1. UNIQUENESS across a scene ----------------------------------------
    // Deliberately gives several entities the SAME NAME: names are no longer an
    // identity mechanism, and duplicates must not be a problem of any kind.
    Scene a;
    {
        std::vector<entt::entity> made;
        for (int i = 0; i < 256; ++i) {
            const entt::entity e = a.CreateEntity(i % 3 == 0 ? "Cube" : "UI Label");
            a.Registry().emplace<Transform>(e, Transform{});
            made.push_back(e);
        }
        // Parent half of them, so the round-trip below exercises parent links too.
        for (usize i = 1; i < made.size(); i += 2)
            a.Registry().emplace<Parent>(made[i], Parent{made[i - 1]});
    }
    const std::vector<u64> g0 = LiveGuids(a);
    c.Expect(g0.size() == 256, "every created entity got a Guid");
    c.Expect(AllUniqueNonZero(g0), "guids are unique and non-zero across a scene");

    // --- 2. STABILITY across save -> load -> save -----------------------------
    const std::string s1 = scene::SaveSceneToString(a);
    scene::SceneData d1;
    c.Expect(scene::ParseSceneString(s1, d1), "snapshot parses");
    c.Expect(d1.entities.size() == 256, "snapshot holds every entity");
    {
        std::vector<u64> parsed;
        for (const scene::EntityData& d : d1.entities) parsed.push_back(d.guid);
        c.Expect(AllUniqueNonZero(parsed), "every serialized entity carries a guid");
        const std::unordered_set<u64> live(g0.begin(), g0.end());
        bool same = parsed.size() == live.size();
        for (const u64 g : parsed) same = same && live.count(g) != 0;
        c.Expect(same, "serialized guids match the live ones exactly");
    }
    Scene b;
    const std::vector<u64> g1 = FakeInstantiate(b, d1);
    {
        const std::unordered_set<u64> live(g0.begin(), g0.end());
        bool same = g1.size() == live.size();
        for (const u64 g : g1) same = same && live.count(g) != 0;
        c.Expect(same, "load into a fresh scene adopts the saved guids");
    }
    const std::string s2 = scene::SaveSceneToString(b);
    scene::SceneData d2;
    c.Expect(scene::ParseSceneString(s2, d2), "re-saved snapshot parses");
    {
        std::unordered_set<u64> first, second;
        for (const scene::EntityData& d : d1.entities) first.insert(d.guid);
        for (const scene::EntityData& d : d2.entities) second.insert(d.guid);
        c.Expect(first == second, "save/load/save leaves the guid set unchanged");
    }
    // A THIRD cycle, to catch a mechanism that is stable once but drifts after.
    Scene b3;
    FakeInstantiate(b3, d2);
    {
        scene::SceneData d3;
        c.Expect(scene::ParseSceneString(scene::SaveSceneToString(b3), d3), "3rd cycle parses");
        std::unordered_set<u64> first, third;
        for (const scene::EntityData& d : d1.entities) first.insert(d.guid);
        for (const scene::EntityData& d : d3.entities) third.insert(d.guid);
        c.Expect(first == third, "guids survive three save/load cycles unchanged");
    }

    // --- 3. FRESHNESS on copy/paste and prefab instantiate --------------------
    // Both go through SaveSubtreeToString -> ParseSceneString -> Instantiate
    // (Editor::CopySelection/PasteSubtree, DuplicateSelection, InstantiatePrefab,
    // RevertPrefabInstance). A .hbprefab file IS BuildSubtreeJson's output, so
    // one test covers both.
    entt::entity root = entt::null;
    for (const entt::entity e : a.Registry().view<const Guid>()) {
        if (!a.Registry().all_of<Parent>(e)) { root = e; break; }
    }
    c.Expect(root != entt::null, "found a subtree root to copy");
    if (root != entt::null) {
        const std::string frag = scene::SaveSubtreeToString(a, root);
        scene::SceneData fd;
        c.Expect(scene::ParseSceneString(frag, fd), "clipboard fragment parses");
        c.Expect(!fd.entities.empty(), "clipboard fragment is non-empty");
        bool anyGuid = false;
        for (const scene::EntityData& d : fd.entities) anyGuid = anyGuid || d.guid != 0;
        c.Expect(!anyGuid, "a copied subtree carries NO guid (BuildSubtreeJson strips it)");

        // Paste it back into the SAME scene, twice. Both pastes must be fresh,
        // distinct from the originals and from each other.
        const std::vector<u64> before = LiveGuids(a);
        const std::vector<u64> p1 = FakeInstantiate(a, fd);
        const std::vector<u64> p2 = FakeInstantiate(a, fd);
        c.Expect(AllUniqueNonZero(p1) && AllUniqueNonZero(p2), "pasted guids are unique");
        c.Expect(Disjoint(before, p1), "paste #1 does not reuse an existing guid");
        c.Expect(Disjoint(before, p2), "paste #2 does not reuse an existing guid");
        c.Expect(Disjoint(p1, p2), "two pastes of one fragment are different objects");
        c.Expect(AllUniqueNonZero(LiveGuids(a)), "scene still globally unique after pastes");
    }

    // --- 3b. ALIAS-PROOFING: a fragment that DOES carry guids ------------------
    // Belt and braces for the single most important correctness property. If a
    // hand-edited .hbprefab, a spawner re-instantiating one cached SceneData, or
    // a future writer ever leaks guids into a duplicated fragment, the Claim must
    // still refuse to alias.
    {
        Scene z;
        scene::SceneData dup;
        dup.entities.resize(4);
        for (int i = 0; i < 4; ++i) {
            dup.entities[static_cast<usize>(i)].name = "Clone";
            dup.entities[static_cast<usize>(i)].guid = 0xABCDEF0123456789ull; // all the SAME
        }
        const std::vector<u64> burst1 = FakeInstantiate(z, dup);
        const std::vector<u64> burst2 = FakeInstantiate(z, dup); // spawner bursts again
        c.Expect(AllUniqueNonZero(burst1), "a burst of identical rows yields distinct guids");
        c.Expect(Disjoint(burst1, burst2), "a second burst aliases nothing from the first");
        c.Expect(AllUniqueNonZero(LiveGuids(z)), "registry-wide uniqueness holds after bursts");
        c.Expect(burst1[0] == 0xABCDEF0123456789ull,
                 "the FIRST claimant of a free guid still adopts it");
    }

    // --- 4. PRE-GUID FILE loaded twice ----------------------------------------
    // The migration case: a .hbscene written before this field existed. Its
    // entities must get guids on load, and the SAME guids every time, or every
    // row of saved state rebinds to a different object on the next launch.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "hbe_guidtest";
        fs::create_directories(dir, ec);
        const fs::path legacy = dir / "PreGuid.hbscene";
        {
            // Hand-written, guid-free, with parent links - i.e. exactly what is
            // on disk today.
            std::ofstream o(legacy, std::ios::binary | std::ios::trunc);
            o << R"({"version":1,"entities":[)"
                 R"({"name":"Cube","transform":{"p":[0,0,0],"r":[1,0,0,0],"s":[1,1,1]}},)"
                 R"({"name":"Cube","parent":0},)"
                 R"({"name":"Sun"},)"
                 R"({"name":"UI Label"},)"
                 R"({"name":"UI Label","parent":3}]})";
        }
        scene::SceneData l1, l2;
        c.Expect(scene::ParseSceneFile(legacy, l1), "pre-guid file parses (load 1)");
        c.Expect(scene::ParseSceneFile(legacy, l2), "pre-guid file parses (load 2)");
        c.Expect(l1.entities.size() == 5 && l2.entities.size() == 5, "pre-guid file has 5 rows");
        std::vector<u64> a1, a2;
        for (const scene::EntityData& d : l1.entities) a1.push_back(d.guid);
        for (const scene::EntityData& d : l2.entities) a2.push_back(d.guid);
        c.Expect(AllUniqueNonZero(a1), "pre-guid entities all get a guid, all distinct");
        c.Expect(a1 == a2, "a pre-guid file loaded twice derives the SAME guids");

        // ...and the derivation is a property of the FILE, not of load order:
        // a second copy of the same bytes under a different name derives
        // different guids, so two legacy scenes loaded together cannot collide.
        const fs::path other = dir / "PreGuidOther.hbscene";
        fs::copy_file(legacy, other, fs::copy_options::overwrite_existing, ec);
        scene::SceneData l3;
        c.Expect(scene::ParseSceneFile(other, l3), "second pre-guid file parses");
        std::vector<u64> a3;
        for (const scene::EntityData& d : l3.entities) a3.push_back(d.guid);
        c.Expect(Disjoint(a1, a3), "two different pre-guid files derive different guids");

        // Once instantiated and saved, the derived guids become real ones and
        // are stable from then on through the ordinary round-trip.
        Scene lg;
        const std::vector<u64> li = FakeInstantiate(lg, l1);
        c.Expect(li == a1, "the derived guids are what actually lands on the entities");
        scene::SceneData l1b;
        c.Expect(scene::ParseSceneString(scene::SaveSceneToString(lg), l1b), "re-save parses");
        std::unordered_set<u64> want(a1.begin(), a1.end()), got;
        for (const scene::EntityData& d : l1b.entities) got.insert(d.guid);
        c.Expect(want == got, "saving a migrated scene writes the derived guids out");

        fs::remove(legacy, ec);
        fs::remove(other, ec);
    }

    // --- 4b. THE RESIDENT UI LAYER IS NEVER SNAPSHOTTED -----------------------
    // A .hbsave is a whole-registry snapshot, but LoadMode::Replace deliberately
    // SPARES the resident UI layer. If the snapshot carried it, a load would
    // restore a SECOND copy of the whole UI - and because the survivors already
    // hold those guids, the Claim would refuse to adopt them and mint fresh,
    // random, per-launch ones instead. So the writer must exclude it outright.
    //
    // TWO spares, and BOTH must be excluded, because they are what the two
    // Replace-sweep predicates spare:
    //   Persistent  - the runtime decoration. Still used by the generated
    //                 world-UI page quads and by a LEGACY `.hbscene` UI layer.
    //   UIDocMember - an open `.hbui` document. This is what production uses
    //                 now: the engine stopped stamping Persistent on the UI
    //                 layer when it started opening it as a document, so the
    //                 production form of this invariant lives on this component.
    {
        Scene ui;
        for (const char* n : {"MenuRoot", "HudRoot"}) {
            const entt::entity e = ui.CreateEntity(n);
            ui.Registry().emplace<Transform>(e);
            ui.Registry().emplace<Persistent>(e);
        }
        // The production shape: a document member with NO Persistent tag at all.
        for (const char* n : {"DocMenuRoot", "DocHudRoot"}) {
            const entt::entity e = ui.CreateEntity(n);
            ui.Registry().emplace<Transform>(e);
            ui.Registry().emplace<UIDocMember>(e, UIDocMember{7u, true});
        }
        const entt::entity world = ui.CreateEntity("Crate");
        ui.Registry().emplace<Transform>(world);

        scene::SceneData snap;
        c.Expect(scene::ParseSceneString(scene::SaveSceneToString(ui), snap),
                 "a registry with resident-UI entities snapshots");
        bool sawPersistent = false, sawDoc = false, sawWorld = false;
        for (const scene::EntityData& d : snap.entities) {
            if (d.name == "MenuRoot" || d.name == "HudRoot") sawPersistent = true;
            if (d.name == "DocMenuRoot" || d.name == "DocHudRoot") sawDoc = true;
            if (d.name == "Crate") sawWorld = true;
        }
        c.Expect(!sawPersistent,
                 "Persistent (legacy resident UI) entities are NOT written into a snapshot");
        c.Expect(!sawDoc,
                 "UIDocMember (.hbui document) entities are NOT written into a snapshot");
        c.Expect(sawWorld && snap.entities.size() == 1,
                 "the snapshot holds exactly the world, with both resident forms excluded");
    }

    // --- 5. FILE IDENTITY inside a real project -------------------------------
    // Everything above runs with NO active project, so NormalizePathKey takes its
    // filename-only FALLBACK. The branch that actually runs in production is the
    // assets-RELATIVE one, and it is the branch the design leans on ("a moved or
    // copied project keeps its guids"). Open a scratch project and prove both
    // halves of that claim, because the fallback would fail each of them:
    //   * two scenes with the SAME filename in different Assets subfolders must
    //     derive DIFFERENT guids (the fallback would alias them outright), and
    //   * the same relative path under a project at a DIFFERENT root must derive
    //     the SAME guids (that is what "moved project" means).
    // This runs last: it leaves a project open, which nothing after it may assume.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec) / "hbe_guidproj";
        fs::remove_all(base, ec);
        const char* kScene = R"({"version":1,"entities":[)"
                             R"({"name":"Cube"},{"name":"Cube","parent":0},{"name":"Sun"}]})";
        const auto writeScene = [&](const fs::path& p) {
            fs::create_directories(p.parent_path(), ec);
            std::ofstream o(p, std::ios::binary | std::ios::trunc);
            o << kScene;
        };
        const auto guidsOf = [&](const fs::path& p) {
            scene::SceneData d;
            std::vector<u64> g;
            if (scene::ParseSceneFile(p, d))
                for (const scene::EntityData& e : d.entities) g.push_back(e.guid);
            return g;
        };

        bool built = Project::Active().Create(base / "ProjA", "ProjA");
        built = built && Project::Active().Create(base / "ProjB", "ProjB");
        c.Expect(built, "scratch projects created");
        if (built) {
            const fs::path aAssets = base / "ProjA" / "Assets";
            const fs::path bAssets = base / "ProjB" / "Assets";
            writeScene(aAssets / "Levels" / "Camp.hbscene");
            writeScene(aAssets / "Backup" / "Camp.hbscene"); // SAME filename
            writeScene(bAssets / "Levels" / "Camp.hbscene"); // same REL path, other root

            c.Expect(Project::Active().Open(base / "ProjA" / "ProjA.hbproj"),
                     "scratch project A opens");
            const std::vector<u64> lv = guidsOf(aAssets / "Levels" / "Camp.hbscene");
            const std::vector<u64> bk = guidsOf(aAssets / "Backup" / "Camp.hbscene");
            c.Expect(lv.size() == 3 && bk.size() == 3, "both same-named scenes parse");
            c.Expect(AllUniqueNonZero(lv), "assets-relative derivation yields guids");
            c.Expect(Disjoint(lv, bk),
                     "same filename in two Assets subfolders derives DIFFERENT guids");
            c.Expect(lv == guidsOf(aAssets / "Levels" / "Camp.hbscene"),
                     "the assets-relative derivation is stable across loads");

            c.Expect(Project::Active().Open(base / "ProjB" / "ProjB.hbproj"),
                     "scratch project B opens");
            c.Expect(lv == guidsOf(bAssets / "Levels" / "Camp.hbscene"),
                     "the same relative path under a MOVED project derives the SAME guids");
        }
        fs::remove_all(base, ec);
    }

    return c.ok;
}

} // namespace hbe::guid
