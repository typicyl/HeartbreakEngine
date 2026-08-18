// Vegetation/VegetationGenTest.cpp - --test-veggen.
//
// Pins P4: both first-class generators are registered and strategy-selected, each is
// DETERMINISTIC (fixed species+seed -> identical skeleton bytes), a different seed varies
// it, the two strategies produce genuinely different structures, the output stays within
// species bounds (branch order, height, has trunk+branches+leaves), and it meshes.
// Headless.
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/TreeMesher.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

#include <algorithm>

namespace hbe::veg {
namespace {

int g_fail = 0;
#define VG_CHECK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) { HBE_ERROR("veggen: FAIL - {}", msg); ++g_fail; }        \
    } while (0)

struct Stats {
    u32 nodes = 0;
    u8 maxOrder = 0;
    f32 maxY = 0.0f;
    u32 trunk = 0, branch = 0, twig = 0, leaves = 0;
};

Stats Analyze(const PlantSkeleton& s) {
    Stats st;
    st.nodes = s.NodeCount();
    for (u32 i = 0; i < s.NodeCount(); ++i) {
        st.maxOrder = std::max(st.maxOrder, s.order[i]);
        st.maxY = std::max(st.maxY, s.pos[i].y);
        switch (static_cast<PlantPart>(s.kind[i])) {
            case PlantPart::Trunk: ++st.trunk; break;
            case PlantPart::Branch: ++st.branch; break;
            case PlantPart::Twig: ++st.twig; break;
            case PlantPart::LeafCluster: ++st.leaves; break;
            default: break;
        }
    }
    return st;
}

void ExerciseGenerator(VegetationWorld& world, IPlantGenerator* gen, const Species& sp,
                       const char* label) {
    if (!gen) { VG_CHECK(false, "generator missing"); return; }
    PlantGenParams p; p.species = SpeciesId{0}; p.seed = 0xBEEF1234u; p.age01 = 1.0f;

    PlantSkeleton a, b, c;
    VG_CHECK(gen->Generate(p, sp, a), "generate A");
    VG_CHECK(gen->Generate(p, sp, b), "generate B (same seed)");
    VG_CHECK(a.Validate(), "skeleton validates");
    VG_CHECK(HashSkeleton(a) == HashSkeleton(b), "deterministic: same seed -> same bytes");

    PlantGenParams p2 = p; p2.seed = 0x0DDBA117u;
    VG_CHECK(gen->Generate(p2, sp, c), "generate C (diff seed)");
    VG_CHECK(HashSkeleton(a) != HashSkeleton(c), "different seed -> different skeleton");

    const Stats st = Analyze(a);
    VG_CHECK(st.nodes > 20, "generator produces real structure (>20 nodes)");
    VG_CHECK(st.nodes < 60000, "node count within the hard cap");
    VG_CHECK(st.maxOrder <= sp.maxBranchOrder, "branch order within species maxBranchOrder");
    VG_CHECK(st.trunk > 0, "has a trunk (order-0 nodes)");
    VG_CHECK(st.branch + st.twig > 0, "has branches");
    VG_CHECK(st.leaves > 0, "has leaf clusters");
    VG_CHECK(st.maxY > sp.maxHeight * 0.4f, "reaches a plausible height");
    VG_CHECK(st.maxY < sp.maxHeight * 1.6f + sp.crownWidth,
             "does not wildly exceed the species height");

    // Integrates with the mesher.
    Model m = BuildTreeMesh(a, sp);
    VG_CHECK(!m.empty() && m[0].VertexCount() > 0, "generated skeleton meshes");
    (void)label;
}

} // namespace

bool GenSelfTest() {
    g_fail = 0;
    VegetationWorld world;

    IPlantGenerator* spacecol = world.Generator("spacecol");
    IPlantGenerator* lsystem = world.Generator("lsystem");
    VG_CHECK(spacecol != nullptr, "'spacecol' generator registered");
    VG_CHECK(lsystem != nullptr, "'lsystem' generator registered");
    VG_CHECK(world.DefaultGenerator() == spacecol, "space colonization is the default generator");

    // Strategy resolution.
    VG_CHECK(world.GeneratorForStrategy(GenStrategy::SpaceColonization) == spacecol,
             "SpaceColonization strategy -> spacecol");
    VG_CHECK(world.GeneratorForStrategy(GenStrategy::LSystem) == lsystem,
             "LSystem strategy -> lsystem");

    Species oak;
    oak.name = "oak";
    oak.maxHeight = 14.0f;
    oak.trunkRadius = 0.4f;
    oak.crownWidth = 8.0f;
    oak.branchDensity = 0.7f;
    oak.maxBranchOrder = 4;
    oak.leafDensity = 1.0f;

    ExerciseGenerator(world, spacecol, oak, "spacecol");
    ExerciseGenerator(world, lsystem, oak, "lsystem");

    // The two strategies must produce genuinely DIFFERENT structures for the same input.
    if (spacecol && lsystem) {
        PlantGenParams p; p.species = SpeciesId{0}; p.seed = 0x1234u; p.age01 = 1.0f;
        PlantSkeleton sc, ls;
        spacecol->Generate(p, oak, sc);
        lsystem->Generate(p, oak, ls);
        VG_CHECK(HashSkeleton(sc) != HashSkeleton(ls),
                 "space colonization and L-system produce different structures");
    }

    if (g_fail == 0) HBE_INFO("veggen: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg
