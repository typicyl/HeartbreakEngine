// Vegetation/VegetationSelfTest.cpp - --test-vegdata.
//
// Pins the P1 foundation so later phases build on a proven data model:
//   * SoA lockstep + skeleton validation,
//   * registry interning (same name -> same id, idempotent update),
//   * DETERMINISM: same (species, seed) -> byte-identical skeleton hash; this is the
//     crown-jewel invariant the whole content pipeline is keyed on,
//   * store AddTree slicing + handle round-trip + Clear,
//   * noise-field determinism.
// Headless (no GPU / window / project), so a build script can gate on it.
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationBackends.h"
#include "Core/Log.h"

namespace hbe::veg {
namespace {

int g_fail = 0;
#define VEG_CHECK(cond, msg)                                                   \
    do {                                                                       \
        if (!(cond)) { HBE_ERROR("vegdata: FAIL - {}", msg); ++g_fail; }       \
    } while (0)

Species MakeOak() {
    Species s;
    s.name = "oak";
    s.maxHeight = 14.0f;
    s.trunkRadius = 0.4f;
    s.branchDensity = 0.7f;
    s.maxBranchOrder = 4;
    return s;
}
Species MakePine() {
    Species s;
    s.name = "pine";
    s.maxHeight = 22.0f;
    s.trunkRadius = 0.3f;
    s.branchDensity = 0.4f;
    s.maxBranchOrder = 3;
    return s;
}

} // namespace

bool DataSelfTest() {
    g_fail = 0;
    VegetationWorld world;

    // --- Registry interning ----------------------------------------------------------
    SpeciesRegistry& reg = world.Species();
    const SpeciesId oak = reg.Add(MakeOak());
    const SpeciesId pine = reg.Add(MakePine());
    VEG_CHECK(oak.Valid() && pine.Valid(), "species ids valid");
    VEG_CHECK(oak.v != pine.v, "distinct species get distinct ids");
    VEG_CHECK(reg.Find("oak") == oak, "Find returns the interned id");
    VEG_CHECK(reg.Add(MakeOak()) == oak, "re-adding a name is idempotent (same id)");
    VEG_CHECK(reg.Count() == 2, "two species interned");
    VEG_CHECK(reg.Get(oak).maxHeight == 14.0f, "Get returns the record");
    VEG_CHECK(!reg.Find("missing").Valid(), "Find of an absent name is invalid");

    // --- Generator present + produces a valid skeleton -------------------------------
    IPlantGenerator* gen = world.Generator("minimal");
    VEG_CHECK(gen != nullptr, "'minimal' generator registered");
    VEG_CHECK(world.DefaultGenerator() != nullptr, "a default generator exists");
    VEG_CHECK(world.Noise("value") != nullptr, "'value' noise registered");
    VEG_CHECK(world.Wind("hierarchical") != nullptr, "'hierarchical' wind registered");
    VEG_CHECK(world.Distribution("null") != nullptr, "'null' distribution registered");

    PlantSkeleton skelA, skelB, skelC;
    if (gen) {
        PlantGenParams p; p.species = oak; p.seed = 0xABCDEF12u; p.age01 = 1.0f;
        VEG_CHECK(gen->Generate(p, reg.Get(oak), skelA), "generate skeleton A");
        VEG_CHECK(skelA.NodeCount() > 4, "skeleton has real structure (>4 nodes)");
        VEG_CHECK(skelA.Validate(), "skeleton A validates (SoA lockstep, parents in range)");
        VEG_CHECK(skelA.parent[0] == -1, "node 0 is a root");

        // DETERMINISM: same seed -> identical bytes.
        VEG_CHECK(gen->Generate(p, reg.Get(oak), skelB), "generate skeleton B (same seed)");
        VEG_CHECK(HashSkeleton(skelA) == HashSkeleton(skelB),
                  "same (species,seed) -> identical skeleton hash");
        VEG_CHECK(skelA.NodeCount() == skelB.NodeCount(), "same seed -> same node count");

        // A different seed should (almost surely) differ.
        PlantGenParams p2 = p; p2.seed = 0x13579BDFu;
        VEG_CHECK(gen->Generate(p2, reg.Get(oak), skelC), "generate skeleton C (diff seed)");
        VEG_CHECK(HashSkeleton(skelA) != HashSkeleton(skelC),
                  "different seed -> different skeleton hash");
    }

    // --- Store: AddTree slices branches, handles round-trip, Clear resets ------------
    VegetationStore store;
    VEG_CHECK(store.Empty(), "fresh store is empty");
    const TreeId t0 = store.AddTree(glm::mat4(1.0f), oak, 0xABCDEF12u, skelA);
    VEG_CHECK(t0.Valid() && t0.v == 0, "first tree handle is 0");
    VEG_CHECK(store.trees.Count() == 1, "one tree row");
    const u32 begin = store.trees.branchBegin[0];
    const u32 count = store.trees.branchCount[0];
    VEG_CHECK(begin == 0, "first tree's branch slice starts at 0");
    VEG_CHECK(count == store.branches.Count(), "branch slice covers all branches");
    VEG_CHECK(store.branches.Count() > 0, "skeleton produced branch segments");
    // Every branch order matches a real skeleton order (>=1: trunk emits no self-segment
    // for its root, and leaf clusters go to FoliageClusters, not branches).
    bool ordersOk = true;
    for (u32 i = 0; i < store.branches.Count(); ++i)
        if (store.branches.order[i] == 0 && store.branches.a[i] == store.branches.b[i])
            ordersOk = false;
    VEG_CHECK(ordersOk, "no degenerate zero-length branch segments");

    const TreeId t1 = store.AddTree(glm::mat4(1.0f), pine, 0x999u, skelC);
    VEG_CHECK(t1.v == 1 && store.trees.Count() == 2, "second tree handle round-trips");
    VEG_CHECK(store.trees.branchBegin[1] == count, "second tree's slice starts after the first");
    VEG_CHECK(store.ApproxBytes() > 0, "store reports a non-zero footprint");

    store.Clear();
    VEG_CHECK(store.Empty(), "Clear resets the store");

    // --- Per-shard stores in the world ------------------------------------------------
    VegetationStore& s10 = world.StoreFor(10);
    s10.AddTree(glm::mat4(1.0f), oak, 1u, skelA);
    VEG_CHECK(world.FindStore(10) == &s10, "StoreFor/FindStore agree");
    VEG_CHECK(world.FindStore(11) == nullptr, "absent shard has no store");
    VEG_CHECK(world.ResidentShardCount() == 1, "one resident shard");
    world.ReleaseStore(10);
    VEG_CHECK(world.FindStore(10) == nullptr, "ReleaseStore evicts");
    VEG_CHECK(world.ResidentShardCount() == 0, "no resident shards after release");

    // --- Noise determinism -----------------------------------------------------------
    if (INoiseField* nz = world.Noise("value")) {
        const f32 a = nz->Sample(12.5f, -7.25f, 42);
        const f32 b = nz->Sample(12.5f, -7.25f, 42);
        VEG_CHECK(a == b, "noise: same (x,z,seed) -> same value");
        VEG_CHECK(a >= 0.0f && a < 1.0f, "noise in [0,1)");
        VEG_CHECK(nz->Sample(12.5f, -7.25f, 43) != a, "noise: seed changes the value");
    }

    if (g_fail == 0) HBE_INFO("vegdata: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg
