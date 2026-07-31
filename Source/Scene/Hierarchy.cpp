// Scene/Hierarchy.cpp - see Hierarchy.h for the contract.
#include "Scene/Hierarchy.h"

#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>

namespace hbe::scene {

std::pair<i32, u32> OrderKey(const entt::registry& reg, entt::entity e) {
    const HierarchyOrder* h = reg.valid(e) ? reg.try_get<const HierarchyOrder>(e) : nullptr;
    // entt::to_entity strips the 12-bit version, so a handle recycled after a delete
    // no longer sorts above every fresh one. It is a tiebreak only.
    return {h ? h->index : std::numeric_limits<i32>::max(), entt::to_entity(e)};
}

bool OrderLess(const entt::registry& reg, entt::entity a, entt::entity b) {
    return OrderKey(reg, a) < OrderKey(reg, b);
}

void SortSiblings(const entt::registry& reg, std::vector<entt::entity>& v) {
    std::sort(v.begin(), v.end(),
              [&reg](entt::entity a, entt::entity b) { return OrderLess(reg, a, b); });
}

void BuildChildrenMap(const entt::registry& reg, ChildrenMap& out, ChildFilter keep) {
    out.clear();
    for (const entt::entity c : reg.view<const Parent>()) {
        const entt::entity p = reg.get<const Parent>(c).entity;
        if (!reg.valid(p)) continue; // dangling link: `c` is a root, not a child
        if (keep && !keep(reg, c)) continue;
        out[static_cast<u32>(p)].push_back(c);
    }
    for (auto& [p, kids] : out) SortSiblings(reg, kids);
}

ChildrenMap BuildChildrenMap(const entt::registry& reg) {
    ChildrenMap m;
    BuildChildrenMap(reg, m);
    return m;
}

std::vector<entt::entity> ChildrenOf(const entt::registry& reg, entt::entity parent) {
    std::vector<entt::entity> kids;
    if (!reg.valid(parent)) return kids;
    for (const entt::entity c : reg.view<const Parent>())
        if (reg.get<const Parent>(c).entity == parent) kids.push_back(c);
    SortSiblings(reg, kids);
    return kids;
}

std::vector<entt::entity> SubtreeInOrder(const ChildrenMap& kids, entt::entity root) {
    std::vector<entt::entity> out;
    if (root == entt::null) return out;
    // LIFO stack, children pushed in REVERSE so they pop in order. (The old walk got
    // the same answer by accident, because entt's reverse-iterating view undid the
    // LIFO; it stopped being an accident the moment the pool was perturbed.)
    std::vector<entt::entity> stack{root};
    // Visited set, because a Parent CYCLE is reachable from a hand-edited `.hbscene`
    // (Instantiate wires parents from raw file-row indices; only Editor::Reparent
    // refuses cycles) and every walk this helper replaced would have hung on one.
    // BuildSubtreeJson's own dedupe is the precedent.
    std::unordered_set<u32> seen;
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!seen.insert(static_cast<u32>(e)).second) continue;
        out.push_back(e);
        const auto it = kids.find(static_cast<u32>(e));
        if (it == kids.end()) continue;
        for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) stack.push_back(*r);
    }
    return out;
}

std::vector<entt::entity> SubtreeInOrder(const entt::registry& reg, entt::entity root) {
    if (!reg.valid(root)) return {};
    return SubtreeInOrder(BuildChildrenMap(reg), root);
}

bool ReorderSibling(entt::registry& reg, entt::entity moved, entt::entity target,
                    bool before) {
    if (moved == target || !reg.valid(moved) || !reg.valid(target)) return false;
    const auto parentOf = [&reg](entt::entity e) -> entt::entity {
        const Parent* p = reg.try_get<const Parent>(e);
        return (p && reg.valid(p->entity)) ? p->entity : entt::null;
    };
    const entt::entity parent = parentOf(moved);
    if (parent != parentOf(target)) return false; // not siblings (roots share null)

    std::vector<entt::entity> sibs;
    if (parent == entt::null) {
        // Roots: every live entity with no (valid) parent, SCOPED THE WAY THE PANEL
        // SCOPES THEM. An unscoped gather is not a misordering (the key is a total
        // order, so a dense renumber preserves relative order) but it WRITES
        // HierarchyOrder on every root in the registry - including roots owned by
        // other additively-streamed `.hbscene` files, whose "order" would then turn
        // up as whole-file diff churn in the next Save All, and `.hbui` document
        // members, whose order is a different mechanism entirely (UISwapOrder).
        // Dragging one root in the active scene must not rewrite another file.
        //
        // The three filters mirror Editor::DrawHierarchy exactly: generated terrain
        // chunks are not hierarchy rows at all; a document member only shares a
        // sibling group with another member of the SAME document; and a root belongs
        // to the group of the scene FILE it came from.
        const auto docOf = [&reg](entt::entity e) -> u32 {
            const UIDocMember* m = reg.try_get<const UIDocMember>(e);
            return m ? m->doc : 0u;
        };
        const auto srcOf = [&reg](entt::entity e) -> const std::string& {
            static const std::string kNone;
            const SceneSource* s = reg.try_get<const SceneSource>(e);
            return s ? s->scene : kNone;
        };
        const u32 wantDoc = docOf(moved);
        const std::string wantSrc = srcOf(moved);
        // Read through a CONST registry ref - Scene.h's note: the const storage<T>()
        // returns a pointer (null when absent), the non-const one returns a reference
        // and creates it.
        const entt::registry& creg = reg;
        const auto* es = creg.storage<entt::entity>();
        if (!es) return false;
        for (const entt::entity e : *es) {
            if (!reg.valid(e) || parentOf(e) != entt::null) continue;
            if (reg.all_of<TerrainChunk>(e)) continue;
            if (docOf(e) != wantDoc) continue;
            if (srcOf(e) != wantSrc) continue;
            sibs.push_back(e);
        }
        SortSiblings(reg, sibs);
    } else {
        sibs = ChildrenOf(reg, parent);
    }

    sibs.erase(std::remove(sibs.begin(), sibs.end(), moved), sibs.end());
    const auto at = std::find(sibs.begin(), sibs.end(), target);
    if (at == sibs.end()) return false;
    sibs.insert(before ? at : at + 1, moved);

    for (usize i = 0; i < sibs.size(); ++i)
        reg.emplace_or_replace<HierarchyOrder>(sibs[i], HierarchyOrder{static_cast<i32>(i)});
    return true;
}

// --- --test-pasteorder --------------------------------------------------------

namespace {

// The subtree's SHAPE as text: "Root(A(A1 A2 A3) B C(C1(C1a C1b) C2))". Compared
// as a whole string so a failure names the entire wrong shape instead of one index,
// and so DEPTH is checked as hard as breadth.
std::string Shape(const entt::registry& reg, const ChildrenMap& kids, entt::entity e) {
    std::string s = reg.all_of<const Name>(e) ? reg.get<const Name>(e).value : "?";
    const auto it = kids.find(static_cast<u32>(e));
    if (it == kids.end() || it->second.empty()) return s;
    s += "(";
    for (usize i = 0; i < it->second.size(); ++i) {
        if (i) s += " ";
        s += Shape(reg, kids, it->second[i]);
    }
    return s + ")";
}

std::string Shape(const Scene& s, entt::entity root) {
    const entt::registry& reg = s.Registry();
    return reg.valid(root) ? Shape(reg, BuildChildrenMap(reg), root) : std::string("<dead>");
}

// EXACTLY what BuildSubtreeJson used to do: a LIFO stack whose children come from a
// full `view<const Parent>` scan, in pool order. Kept verbatim so the test can prove
// the two orders DISAGREE on the perturbed registry below - i.e. that the fix is
// being measured, not the status quo.
std::string LegacyShape(const entt::registry& reg, entt::entity root) {
    std::string s = reg.all_of<const Name>(root) ? reg.get<const Name>(root).value : "?";
    std::vector<entt::entity> kids;
    for (const entt::entity c : reg.view<const Parent>())
        if (reg.get<const Parent>(c).entity == root) kids.push_back(c);
    // The old walk pushed these onto a LIFO, so they came back out reversed.
    std::reverse(kids.begin(), kids.end());
    if (kids.empty()) return s;
    s += "(";
    for (usize i = 0; i < kids.size(); ++i) {
        if (i) s += " ";
        s += LegacyShape(reg, kids[i]);
    }
    return s + ")";
}

entt::entity Kid(Scene& s, entt::entity parent, const char* name) {
    const entt::entity e = s.CreateEntity(name);
    s.Registry().emplace<Transform>(e, Transform{});
    if (parent != entt::null) s.Registry().emplace<Parent>(e, Parent{parent});
    return e;
}

// PasteSubtree's creation half, verbatim minus the editor-only concerns (the UI
// refusal, StageAssets - these entities reference no assets - and selection).
// Returns the pasted ROOT.
entt::entity Paste(Scene& s, Renderer& r, const std::string& frag) {
    SceneData data;
    if (!ParseSceneString(frag, data) || data.entities.empty()) return entt::null;
    StagedAssets staged;
    std::vector<entt::entity> created;
    Instantiate(s, r, data, staged, LoadMode::Additive, &created);
    if (created.empty()) return entt::null;
    // The one editor policy that IS part of the ordering contract and so has to be
    // modelled here: Editor::PasteSubtree re-mints the pasted ROOT's order so a
    // clone appears at the END of its new sibling group rather than in the middle
    // of it. Descendants keep the fragment's values, which is what preserves shape.
    s.Registry().emplace_or_replace<HierarchyOrder>(created.front(),
                                                    HierarchyOrder{s.NextHierarchyOrder()});
    return created.front();
}

// EVERY root named `name`, as shapes. Sections that paste into the SOURCE scene
// (which is what Ctrl+D does) end up with two identically named roots, and the
// point is that BOTH read the same - so the test asserts on the whole set rather
// than picking one arbitrarily.
std::vector<std::string> RootShapes(const Scene& s, const char* name) {
    std::vector<std::string> out;
    const entt::registry& reg = s.Registry();
    const ChildrenMap kids = BuildChildrenMap(reg);
    std::vector<entt::entity> roots;
    for (const entt::entity e : reg.view<const Name>()) {
        if (reg.get<const Name>(e).value != name) continue;
        const Parent* p = reg.try_get<const Parent>(e);
        if (p && reg.valid(p->entity)) continue;
        roots.push_back(e);
    }
    SortSiblings(reg, roots);
    for (const entt::entity e : roots) out.push_back(Shape(reg, kids, e));
    return out;
}

std::vector<u64> GuidsOfSubtree(const Scene& s, entt::entity root) {
    std::vector<u64> out;
    for (const entt::entity e : SubtreeInOrder(s.Registry(), root))
        if (const Guid* g = s.Registry().try_get<const Guid>(e)) out.push_back(g->value);
    return out;
}

} // namespace

bool PasteOrderSelfTest() {
    namespace fs = std::filesystem;
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("pasteorder: FAILED - {}", what);
        }
    };
    const auto expectEq = [&](const std::string& got, const std::string& want,
                              const char* what) {
        if (got != want) {
            ok = false;
            HBE_ERROR("pasteorder: FAILED - {}\n    want: {}\n    got : {}", what, want, got);
        }
    };

    // No GPU: a device-less Renderer's uploads return invalid handles, the same
    // headless contract --test-sceneslice keeps.
    Renderer renderer;

    // --- 1. A subtree that is both WIDE and DEEP ------------------------------
    // Wide catches an order that is reversed or pool-shuffled; deep catches a walk
    // that only gets the first level right. Both in one tree, one Ctrl+C.
    Scene a;
    const entt::entity root = Kid(a, entt::null, "Root");
    std::string wantShape;
    {
        const entt::entity wide = Kid(a, root, "Wide");
        for (int i = 0; i < 32; ++i) Kid(a, wide, ("W" + std::to_string(i)).c_str());
        const entt::entity mid = Kid(a, root, "Mid");
        const entt::entity m1 = Kid(a, mid, "M1");
        Kid(a, m1, "M1a");
        Kid(a, m1, "M1b");
        Kid(a, mid, "M2");
        // A 16-deep chain: every level is a sibling group of exactly one, which is
        // where an off-by-one in the stack walk shows up as a truncated subtree.
        entt::entity chain = Kid(a, root, "Deep");
        for (int i = 0; i < 16; ++i) chain = Kid(a, chain, ("D" + std::to_string(i)).c_str());
        Kid(a, root, "Last");
        wantShape = Shape(a, root);
    }
    expect(wantShape.find("W0 W1 W2") != std::string::npos,
           "the authored wide group reads W0 W1 W2 ... (sanity on the fixture)");

    {
        const entt::entity paste = Paste(a, renderer, SaveSubtreeToString(a, root));
        expect(paste != entt::null, "a copied subtree pastes");
        expectEq(Shape(a, paste), wantShape, "paste reproduces the source shape (wide + deep)");
    }

    // --- 2. THE CASE THE OLD WALK GETS WRONG ----------------------------------
    // Everything above passes on the OLD code too, because entt's reverse-iterating
    // view and the LIFO stack cancel out on a registry nobody has edited. They stop
    // cancelling the moment the Parent pool is perturbed - and `swap_and_pop` (one
    // component erase) plus handle recycling (one destroy) both perturb it, which is
    // what an ordinary editing session is made of.
    {
        Scene s;
        // Burn six handles and free them, so everything created next comes off the
        // free list with a bumped version - the recycled handles that made the
        // Hierarchy panel's raw-handle sort scramble a pasted subtree.
        std::vector<entt::entity> churn;
        for (int i = 0; i < 6; ++i) churn.push_back(Kid(s, entt::null, "Scratch"));
        for (const entt::entity e : churn) s.Registry().destroy(e);

        const entt::entity g = Kid(s, entt::null, "Group");
        const entt::entity A = Kid(s, g, "A");
        const entt::entity B = Kid(s, g, "B");
        Kid(s, g, "C");
        Kid(s, g, "D");
        Kid(s, g, "E");
        // Unparent-and-reparent B: `remove<Parent>` swap_and_pops the LAST entry into
        // B's slot and the re-emplace appends B at the end, so the Parent pool now
        // disagrees with authored order by two positions - with no authored change.
        s.Registry().remove<Parent>(B);
        s.Registry().emplace<Parent>(B, Parent{g});
        // Same again on A, to move the disagreement to the head of the list.
        s.Registry().remove<Parent>(A);
        s.Registry().emplace<Parent>(A, Parent{g});

        const std::string want = "Group(A B C D E)";
        expectEq(Shape(s, g), want, "authored order is unchanged by pool perturbation");
        // The discriminator: if these MATCHED, this section would prove nothing.
        expect(LegacyShape(s.Registry(), g) != want,
               "the old pool-order walk really does disagree here (legacy: " +
                   LegacyShape(s.Registry(), g) + ")");
        // And the raw-handle sort - what the Hierarchy panel used - is wrong too.
        {
            std::vector<entt::entity> kids = ChildrenOf(s.Registry(), g);
            std::sort(kids.begin(), kids.end(), [](entt::entity x, entt::entity y) {
                return static_cast<u32>(x) < static_cast<u32>(y);
            });
            std::string byHandle;
            for (const entt::entity e : kids)
                byHandle += (byHandle.empty() ? "" : " ") + s.Registry().get<Name>(e).value;
            expect(byHandle != "A B C D E",
                   "the old raw-handle sort really does disagree here (got: " + byHandle + ")");
        }

        const entt::entity paste = Paste(s, renderer, SaveSubtreeToString(s, g));
        expect(paste != entt::null, "the perturbed subtree pastes");
        expectEq(Shape(s, paste), want, "paste follows AUTHORED order, not pool order");

        // --- 2b. fresh identity is unchanged by all of this --------------------
        {
            SceneData fd;
            expect(ParseSceneString(SaveSubtreeToString(s, g), fd), "fragment parses");
            bool anyGuid = false;
            for (const EntityData& d : fd.entities) anyGuid = anyGuid || d.guid != 0;
            expect(!anyGuid, "the fragment still carries NO guid (guid strip intact)");
            const std::vector<u64> src = GuidsOfSubtree(s, g);
            const std::vector<u64> dst = GuidsOfSubtree(s, paste);
            expect(src.size() == dst.size() && !src.empty(), "clone has the same node count");
            std::unordered_set<u64> srcSet(src.begin(), src.end()), dstSet(dst.begin(), dst.end());
            expect(dstSet.size() == dst.size(), "clone guids are unique");
            bool disjoint = true;
            for (const u64 x : dst) disjoint = disjoint && srcSet.count(x) == 0 && x != 0;
            expect(disjoint, "clone mints FRESH guids (nothing aliases the source)");
        }

        // --- 2c. a copy of the clone is the same shape again -------------------
        const entt::entity twice = Paste(s, renderer, SaveSubtreeToString(s, paste));
        expectEq(Shape(s, twice), want, "copying the clone is a fixed point");
    }

    // --- 3. The PREFAB path (fragment via a FILE) -----------------------------
    // Same writer, but through disk, and instantiated into a DIFFERENT scene - the
    // case where none of the source's entity handles exist at all.
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "hbe_pasteorder";
        fs::create_directories(dir, ec);
        const fs::path pf = dir / "Order.hbprefab";
        {
            const std::string frag = SaveSubtreeToString(a, root);
            std::ofstream o(pf, std::ios::binary | std::ios::trunc);
            expect(static_cast<bool>(o), "scratch .hbprefab opens for writing");
            o.write(frag.data(), static_cast<std::streamsize>(frag.size()));
        }
        std::ifstream in(pf, std::ios::binary);
        const std::string frag((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        expect(!frag.empty(), "the .hbprefab reads back");
        Scene other;
        const entt::entity inst = Paste(other, renderer, frag);
        expect(inst != entt::null, "the prefab instantiates into a fresh scene");
        expectEq(Shape(other, inst), wantShape, "prefab instantiate preserves order");
        fs::remove_all(dir, ec);
    }

    // --- 4. A FULL scene save/load keeps order ---------------------------------
    // The copy path is not the only one an author sees: Ctrl+S then reopen must show
    // the same tree. This is also the half a raw-pool-order writer cannot hold, since
    // BuildSceneJson's ROW order is still derived from the (perturbable) entity pool.
    {
        // `a` holds the original AND section 1's clone, both rooted at "Root".
        const std::vector<std::string> want = RootShapes(a, "Root");
        expect(want.size() == 2, "the source scene holds the original and its clone");
        for (const std::string& sh : want)
            expectEq(sh, wantShape, "both live roots read the authored shape");

        SceneData d;
        expect(ParseSceneString(SaveSceneToString(a), d), "full-scene snapshot parses");
        Scene b;
        StagedAssets staged;
        Instantiate(b, renderer, d, staged, LoadMode::Replace);
        const std::vector<std::string> got = RootShapes(b, "Root");
        expect(got.size() == want.size(), "the reloaded scene has both roots");
        for (const std::string& sh : got)
            expectEq(sh, wantShape, "save -> load preserves order at every depth");
        // ...and a second cycle, to catch a mechanism that is stable once then drifts.
        SceneData d2;
        expect(ParseSceneString(SaveSceneToString(b), d2), "second snapshot parses");
        Scene c;
        Instantiate(c, renderer, d2, staged, LoadMode::Replace);
        const std::vector<std::string> got2 = RootShapes(c, "Root");
        expect(got2.size() == want.size(), "two cycles keep both roots");
        for (const std::string& sh : got2)
            expectEq(sh, wantShape, "order survives two save/load cycles");
    }

    // --- 5. MIGRATION: a file written before "order" existed -------------------
    // The contract is that absence reproduces TODAY's behaviour exactly, which is
    // the file's array order. No flag, no rewrite, nothing to migrate.
    {
        const std::string legacy =
            R"({"version":1,"entities":[)"
            R"({"name":"G","transform":{"p":[0,0,0],"r":[1,0,0,0],"s":[1,1,1]}},)"
            R"({"name":"first","parent":0},)"
            R"({"name":"second","parent":0},)"
            R"({"name":"third","parent":0},)"
            R"({"name":"fourth","parent":0}]})";
        SceneData d;
        expect(ParseSceneString(legacy, d), "a pre-'order' fragment parses");
        for (const EntityData& ed : d.entities)
            expect(ed.order == -1, "absent 'order' parses as -1 (the migration sentinel)");
        Scene s;
        const entt::entity g = Paste(s, renderer, legacy);
        expectEq(Shape(s, g), "G(first second third fourth)",
                 "a pre-'order' file keeps its FILE ORDER");
        // And re-copying it now writes the field, with the same result.
        const entt::entity again = Paste(s, renderer, SaveSubtreeToString(s, g));
        expectEq(Shape(s, again), "G(first second third fourth)",
                 "the migrated subtree round-trips unchanged");
    }

    // --- 6. Explicit REORDER is authored data ---------------------------------
    // The drag-to-reorder affordance in the Hierarchy panel calls exactly this, and
    // the result has to survive a copy and a save like any other authored value.
    {
        Scene s;
        const entt::entity g = Kid(s, entt::null, "G");
        const entt::entity A = Kid(s, g, "A");
        Kid(s, g, "B");
        Kid(s, g, "C");
        const entt::entity D = Kid(s, g, "D");
        expectEq(Shape(s, g), "G(A B C D)", "reorder fixture starts in creation order");
        expect(ReorderSibling(s.Registry(), D, A, /*before*/ true), "D moves before A");
        expectEq(Shape(s, g), "G(D A B C)", "reorder takes effect");
        const entt::entity clone = Paste(s, renderer, SaveSubtreeToString(s, g));
        expectEq(Shape(s, clone), "G(D A B C)", "an authored reorder survives copy/paste");
        SceneData d;
        expect(ParseSceneString(SaveSceneToString(s), d), "reordered scene snapshots");
        Scene t;
        StagedAssets staged;
        Instantiate(t, renderer, d, staged, LoadMode::Replace);
        const std::vector<std::string> got = RootShapes(t, "G");
        expect(got.size() == 2, "both the reordered group and its clone reload");
        for (const std::string& sh : got)
            expectEq(sh, "G(D A B C)", "an authored reorder survives save/load");
    }

    // --- 7. A newly created entity sorts LAST ---------------------------------
    // The rule that makes the counter usable: create, and it appears where the author
    // is looking. Checked AFTER a load, which is where a naive counter would collide
    // with values the file already used.
    {
        Scene s;
        const entt::entity g = Paste(
            s, renderer,
            R"({"version":1,"entities":[{"name":"G","order":7},)"
            R"({"name":"x","parent":0,"order":3},{"name":"y","parent":0,"order":9}]})");
        expectEq(Shape(s, g), "G(x y)", "explicit 'order' values drive the sort");
        Kid(s, g, "z");
        expectEq(Shape(s, g), "G(x y z)", "a new child lands after loaded siblings");
    }

    // --- 8. Reordering a ROOT touches only ITS OWN group ----------------------
    // The root branch of ReorderSibling gathers "every live entity with no parent",
    // and its dense renumber WRITES HierarchyOrder on all of them. Unscoped, dragging
    // one root in the active scene rewrites "order" on roots belonging to other
    // additively-streamed `.hbscene` files (whole-file diff churn on the next Save
    // All) and on `.hbui` document members, whose order is a different mechanism
    // entirely. The panel scopes its root list by exactly these three rules, so the
    // reorder has to as well.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity r0 = Kid(s, entt::null, "R0");
        const entt::entity r1 = Kid(s, entt::null, "R1");
        const entt::entity r2 = Kid(s, entt::null, "R2");
        // A root from ANOTHER scene file, and a generated terrain chunk. Neither is a
        // sibling of the three above in the panel, so neither may be renumbered.
        const entt::entity other = Kid(s, entt::null, "Other");
        reg.emplace<SceneSource>(other, SceneSource{"Streamed.hbscene"});
        const entt::entity chunk = Kid(s, entt::null, "Chunk");
        reg.emplace<TerrainChunk>(chunk, TerrainChunk{});
        const i32 otherOrder = reg.get<HierarchyOrder>(other).index;
        const i32 chunkOrder = reg.get<HierarchyOrder>(chunk).index;

        expect(ReorderSibling(reg, r2, r0, /*before*/ true), "a root moves before another root");
        std::vector<entt::entity> roots{r0, r1, r2};
        SortSiblings(reg, roots);
        std::string got;
        for (const entt::entity e : roots)
            got += (got.empty() ? "" : " ") + reg.get<Name>(e).value;
        expectEq(got, "R2 R0 R1", "the active scene's roots reorder");
        expect(reg.get<HierarchyOrder>(other).index == otherOrder,
               "a root belonging to ANOTHER scene file is not renumbered");
        expect(reg.get<HierarchyOrder>(chunk).index == chunkOrder,
               "a generated terrain chunk is not renumbered");
        // ...and a cross-group drop is refused outright rather than silently merging
        // the two groups into one renumbered list.
        expect(!ReorderSibling(reg, other, r0, /*before*/ true),
               "a root from another scene file is not a sibling and the move is refused");
    }

    // --- 9. A TIE is ambiguous, so a reparent must re-mint --------------------
    // HierarchyOrder is compared only within a sibling group, and a reparent is the
    // one operation that makes two groups' values meet: two instances of one
    // `.hbprefab` carry identical child orders (PasteSubtree re-mints the ROOT only),
    // so dragging a child of one under the other produced a DUPLICATE. A tie falls
    // through to entt::to_entity - the entity index, which is assigned from the FILE
    // ROW at load - so a tied pair can swap places across a save/reopen. Editor::
    // Reparent now stamps a fresh index; this section pins the property that makes
    // that fix correct, on the Scene layer where it is testable headlessly.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity g = Kid(s, entt::null, "G");
        const entt::entity a = Kid(s, g, "a");
        const entt::entity b = Kid(s, g, "b");
        // The collision a cross-instance reparent used to leave behind. Taken FROM the
        // scene's own allocator, because that is where a real duplicate comes from -
        // two prefab instances replaying one file's values - and because the re-mint
        // below has to be provably above it.
        const i32 dup = s.NextHierarchyOrder();
        reg.emplace_or_replace<HierarchyOrder>(a, HierarchyOrder{dup});
        reg.emplace_or_replace<HierarchyOrder>(b, HierarchyOrder{dup});
        expect(reg.get<HierarchyOrder>(a).index == reg.get<HierarchyOrder>(b).index,
               "the fixture really does hold two siblings on one index");

        // What Reparent does now: the moved child takes a fresh monotonic index, so it
        // lands LAST in its new group and the tie is gone.
        reg.emplace_or_replace<HierarchyOrder>(b, HierarchyOrder{s.NextHierarchyOrder()});
        expect(reg.get<HierarchyOrder>(a).index != reg.get<HierarchyOrder>(b).index,
               "re-minting on reparent removes the duplicate");
        expectEq(Shape(s, g), "G(a b)", "the re-minted child sorts last in its new group");

        // And the group is now stable across two save/load cycles - which a tied pair
        // is not, because the tiebreak is re-derived from file row order every load.
        StagedAssets staged;
        SceneData d1;
        expect(ParseSceneString(SaveSceneToString(s), d1), "the re-minted scene snapshots");
        Scene s1;
        Instantiate(s1, renderer, d1, staged, LoadMode::Replace);
        SceneData d2;
        expect(ParseSceneString(SaveSceneToString(s1), d2), "...and snapshots again");
        Scene s2;
        Instantiate(s2, renderer, d2, staged, LoadMode::Replace);
        for (const Scene* t : {&s1, &s2}) {
            const std::vector<std::string> sh = RootShapes(*t, "G");
            expect(sh.size() == 1, "the group reloads");
            if (!sh.empty()) expectEq(sh[0], "G(a b)", "no tie survives to be re-broken");
        }
    }

    return ok;
}

} // namespace hbe::scene
