// Vegetation/VegetationDamage.cpp - damage/health + structural support + life-cycle test.
#include "Vegetation/VegetationDamage.h"
#include "Vegetation/VegetationStore.h"
#include "Vegetation/VegetationGrowth.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationSystem.h" // HashSkeleton
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::veg {

f32 DamageTree(VegetationStore& store, TreeId tree, f32 amount) {
    if (!tree.Valid() || tree.v >= store.trees.Count()) return 0.0f;
    f32& h = store.trees.health[tree.v];
    h = glm::clamp(h - amount, 0.0f, 1.0f);
    return h;
}

void BreakBranch(VegetationStore& store, BranchId branch) {
    if (branch.Valid() && branch.v < store.branches.Count()) store.branches.broken[branch.v] = 1u;
}

u32 ComputeBranchSupport(const VegetationStore& store, TreeId tree,
                         std::vector<u8>* outSupported) {
    if (!tree.Valid() || tree.v >= store.trees.Count()) return 0;
    const u32 begin = store.trees.branchBegin[tree.v];
    const u32 count = store.trees.branchCount[tree.v];
    std::vector<u8> sup(count, 0);
    u32 fallen = 0;
    // Branches are in topological order (a parent segment is created before its children),
    // so one forward pass suffices: a branch is supported iff it is unbroken AND its parent
    // is a ground-connected base (-1) or an already-supported branch.
    for (u32 l = 0; l < count; ++l) {
        const u32 g = begin + l;
        const i32 par = store.branches.parent[g];
        bool baseOk;
        if (par < 0) {
            baseOk = true; // ground-connected base segment
        } else if (par >= static_cast<i32>(begin) && par < static_cast<i32>(begin + count)) {
            baseOk = sup[par - begin] != 0;
        } else {
            baseOk = false; // parent outside the slice (malformed) -> unsupported
        }
        sup[l] = (store.branches.broken[g] == 0 && baseOk) ? 1u : 0u;
        if (!sup[l]) ++fallen;
    }
    if (outSupported) *outSupported = std::move(sup);
    return fallen;
}

namespace {
int g_fail = 0;
#define VL_CHECK(cond, msg) do { if (!(cond)) { HBE_ERROR("veglife: FAIL - {}", msg); ++g_fail; } } while (0)

Species TestOak() {
    Species s;
    s.name = "oak";
    s.maxHeight = 12.0f;
    s.crownWidth = 8.0f;
    s.branchDensity = 0.7f;
    s.maxBranchOrder = 4;
    s.leafDensity = 1.0f;
    return s;
}
} // namespace

bool LifeSelfTest() {
    g_fail = 0;
    VegetationWorld world;
    const Species oak = TestOak();
    IPlantGenerator* gen = world.Generator("spacecol");
    VL_CHECK(gen != nullptr, "generator available");

    // --- Incremental growth: young skeleton grows structure while preserving its form ---
    PlantSkeleton young;
    if (gen) {
        PlantGenParams p; p.species = SpeciesId{0}; p.seed = 0x11117777u; p.age01 = 0.4f;
        gen->Generate(p, oak, young);
    }
    const u32 n0 = young.NodeCount();
    // Compare WOODY radius (leaf clumps are larger and unchanged by growth, so they would
    // hide the trunk-thickening in an all-node max).
    auto maxWoody = [](const PlantSkeleton& s) {
        f32 m = 0.0f;
        for (u32 i = 0; i < s.NodeCount(); ++i) {
            const PlantPart k = static_cast<PlantPart>(s.kind[i]);
            if (k == PlantPart::Trunk || k == PlantPart::Branch || k == PlantPart::Twig)
                m = glm::max(m, s.radius[i]);
        }
        return m;
    };
    const f32 maxR0 = maxWoody(young);
    std::vector<glm::vec3> preservedPos(young.pos.begin(), young.pos.begin() + n0);

    PlantSkeleton grown = young;               // copy, then grow it
    GrowSkeleton(grown, oak, 0.4f, 1.0f);
    VL_CHECK(grown.NodeCount() > n0, "growth ADDS nodes (structure, not scale)");
    VL_CHECK(grown.Validate(), "grown skeleton still validates");
    bool preserved = true;
    for (u32 i = 0; i < n0; ++i)
        if (grown.pos[i] != preservedPos[i]) preserved = false;
    VL_CHECK(preserved, "growth PRESERVES the earlier form (original nodes unchanged)");
    VL_CHECK(maxWoody(grown) > maxR0, "growth thickens the wood");

    // Determinism: growing twice is identical.
    PlantSkeleton grownB = young;
    GrowSkeleton(grownB, oak, 0.4f, 1.0f);
    VL_CHECK(HashSkeleton(grown) == HashSkeleton(grownB), "growth is deterministic");

    // --- Damage + structural support -------------------------------------------------
    VegetationStore store;
    const TreeId t = store.AddTree(glm::mat4(1.0f), SpeciesId{0}, 0x11117777u, grown);
    VL_CHECK(store.branches.Count() > 4, "tree has a branch graph");

    // Intact: nothing broken -> nothing fallen (every branch reaches a base).
    VL_CHECK(ComputeBranchSupport(store, t) == 0, "intact tree: no fallen branches");

    // Find a branch that HAS children (so breaking it must orphan something).
    const u32 begin = store.trees.branchBegin[t.v];
    const u32 count = store.trees.branchCount[t.v];
    i32 breakG = -1;
    for (u32 g = begin; g < begin + count && breakG < 0; ++g)
        for (u32 c = begin; c < begin + count; ++c)
            if (store.branches.parent[c] == static_cast<i32>(g)) { breakG = static_cast<i32>(g); break; }
    VL_CHECK(breakG >= 0, "found a branch with descendants to break");

    if (breakG >= 0) {
        BreakBranch(store, BranchId{static_cast<u32>(breakG)});
        const u32 fallen = ComputeBranchSupport(store, t);
        VL_CHECK(fallen >= 2, "breaking a branch orphans it AND its descendants (>=2 fallen)");
    }

    // Health: damage reduces it; enough damage kills the tree.
    VL_CHECK(store.trees.health[t.v] == 1.0f, "tree starts healthy");
    const f32 h1 = DamageTree(store, t, 0.3f);
    VL_CHECK(h1 < 1.0f && h1 > 0.0f, "damage reduces health");
    const f32 h2 = DamageTree(store, t, 5.0f);
    VL_CHECK(h2 == 0.0f, "enough damage kills the tree (health clamps to 0)");

    if (g_fail == 0) HBE_INFO("veglife: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg
