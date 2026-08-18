// Vegetation/VegetationGrowthTest.cpp - --test-veggrow.
//
// Pins STRUCTURAL growth: generating the same species+seed at increasing age produces a
// genuinely more-developed skeleton (more nodes/branches + taller), NOT a scaled copy of
// a fixed structure - the property the design calls out as the difference between real
// growth and mesh scaling. Also re-confirms determinism at each age. Headless.
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Core/Log.h"

#include <algorithm>

namespace hbe::veg {
namespace {

int g_fail = 0;
#define GR_CHECK(cond, msg)                                                    \
    do {                                                                       \
        if (!(cond)) { HBE_ERROR("veggrow: FAIL - {}", msg); ++g_fail; }       \
    } while (0)

struct Stats {
    u32 nodes = 0;
    u32 branches = 0; // Branch + Twig nodes
    f32 maxY = 0.0f;
};

Stats Analyze(const PlantSkeleton& s) {
    Stats st;
    st.nodes = s.NodeCount();
    for (u32 i = 0; i < s.NodeCount(); ++i) {
        st.maxY = std::max(st.maxY, s.pos[i].y);
        const PlantPart k = static_cast<PlantPart>(s.kind[i]);
        if (k == PlantPart::Branch || k == PlantPart::Twig) ++st.branches;
    }
    return st;
}

void ExerciseGrowth(IPlantGenerator* gen, const Species& sp, const char* label) {
    if (!gen) { GR_CHECK(false, "generator missing"); return; }
    const f32 ages[] = {0.2f, 0.45f, 0.7f, 1.0f};
    const u64 seed = 0x6C0FFEEull;

    Stats prev;
    bool first = true;
    Stats young, mature;
    for (usize a = 0; a < 4; ++a) {
        PlantGenParams p; p.species = SpeciesId{0}; p.seed = seed; p.age01 = ages[a];
        PlantSkeleton s, s2;
        GR_CHECK(gen->Generate(p, sp, s), "generate at age");
        // Determinism at this age.
        GR_CHECK(gen->Generate(p, sp, s2), "regenerate at age");
        GR_CHECK(HashSkeleton(s) == HashSkeleton(s2), "deterministic at each age");

        const Stats st = Analyze(s);
        if (a == 0) young = st;
        if (a == 3) mature = st;
        if (!first) {
            // Height grows with age (a plant gets taller as it matures). Node count is
            // compared endpoint-to-endpoint below rather than between adjacent ages, since
            // adjacent ages can wobble by a few nodes from the stochastic attractor fill.
            GR_CHECK(st.maxY > prev.maxY - 1e-3f, "height grows with age");
        }
        prev = st;
        first = false;
    }

    // The decisive check: a MATURE plant has substantially MORE structure than a YOUNG one
    // - so age adds branches/nodes (structural growth), it is not one fixed tree scaled up.
    GR_CHECK(mature.nodes > young.nodes * 3 / 2, "mature plant has substantially more nodes than young");
    GR_CHECK(mature.branches > young.branches, "mature plant has more branches than young");
    GR_CHECK(mature.maxY > young.maxY, "mature plant is taller than young");
    (void)label;
}

} // namespace

bool GrowthSelfTest() {
    g_fail = 0;
    VegetationWorld world;

    Species oak;
    oak.name = "oak";
    oak.maxHeight = 14.0f;
    oak.trunkRadius = 0.4f;
    oak.crownWidth = 8.0f;
    oak.branchDensity = 0.75f;
    oak.maxBranchOrder = 4;
    oak.leafDensity = 1.0f;

    ExerciseGrowth(world.Generator("spacecol"), oak, "spacecol");
    ExerciseGrowth(world.Generator("lsystem"), oak, "lsystem");

    if (g_fail == 0) HBE_INFO("veggrow: all checks passed");
    return g_fail == 0;
}

} // namespace hbe::veg
