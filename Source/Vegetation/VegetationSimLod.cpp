// Vegetation/VegetationSimLod.cpp - simulation-LOD tier assignment (P7).
//
// Assigns each tree in a per-shard store a SimTier from its camera distance. Tiers are
// INDEPENDENT of render LOD: they bound how much SIMULATION (wind detail, growth, per-leaf
// work) a plant gets. Promotion toward higher detail is BUDGETED per call so a camera
// sweep across a dense forest cannot spike the frame promoting everything at once;
// demotions (toward cheaper tiers) are immediate. See docs/Design-Vegetation.md section 4.
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationStore.h"
#include "Core/Log.h"

#include <glm/glm.hpp>

namespace hbe::veg {

namespace {
SimTier TierForDistance(f32 d, const SimLodConfig& c) {
    if (d < c.heroDist) return SimTier::Hero;
    if (d < c.nearDist) return SimTier::Near;
    if (d < c.midDist) return SimTier::Mid;
    if (d < c.farDist) return SimTier::Far;
    return SimTier::Impostor;
}
} // namespace

void AssignSimTiers(VegetationStore& store, const glm::vec3& camPos, const SimLodConfig& cfg) {
    u32 promotions = 0;
    const u32 n = store.trees.Count();
    for (u32 i = 0; i < n; ++i) {
        const glm::vec3 p = glm::vec3(store.trees.xform[i][3]);
        const f32 d = glm::distance(p, camPos);
        const u8 want = static_cast<u8>(TierForDistance(d, cfg));
        const u8 cur = store.trees.simTier[i];
        if (want < cur) {
            // Higher detail (lower enum) - budgeted so a sweep does not spike.
            if (promotions >= cfg.promotionBudget) continue;
            store.trees.simTier[i] = want;
            ++promotions;
        } else if (want > cur) {
            store.trees.simTier[i] = want; // cheaper tier: demote immediately
        }
    }
}

bool LodSelfTest() {
    int fail = 0;
    auto check = [&](bool c, const char* m) { if (!c) { HBE_ERROR("veglod: FAIL - {}", m); ++fail; } };

    // Five trees strung out along +X at increasing distance from the origin camera.
    VegetationStore store;
    PlantSkeleton empty; // an (empty) skeleton is fine - the tier logic only reads xforms
    const f32 dists[5] = {5.0f, 20.0f, 50.0f, 100.0f, 200.0f};
    for (f32 dx : dists) {
        glm::mat4 x(1.0f);
        x[3] = glm::vec4(dx, 0.0f, 0.0f, 1.0f);
        store.AddTree(x, SpeciesId{0}, 0, empty);
    }
    // Everything starts at the store's default (Far). Give it enough calls (budget 4) for
    // all promotions to settle, then check the bands.
    SimLodConfig cfg; // hero<8, near<25, mid<60, far<120, else impostor
    const glm::vec3 cam(0.0f);
    for (int i = 0; i < 8; ++i) AssignSimTiers(store, cam, cfg);

    check(store.trees.simTier[0] == static_cast<u8>(SimTier::Hero), "5m -> Hero");
    check(store.trees.simTier[1] == static_cast<u8>(SimTier::Near), "20m -> Near");
    check(store.trees.simTier[2] == static_cast<u8>(SimTier::Mid), "50m -> Mid");
    check(store.trees.simTier[3] == static_cast<u8>(SimTier::Far), "100m -> Far");
    check(store.trees.simTier[4] == static_cast<u8>(SimTier::Impostor), "200m -> Impostor");

    // Promotion budget: force all five to want Hero, from a cold Impostor start; with
    // budget 1 exactly ONE should promote per call.
    VegetationStore s2;
    for (int i = 0; i < 5; ++i) {
        glm::mat4 x(1.0f);
        s2.AddTree(x, SpeciesId{0}, 0, empty);      // all at the origin -> all want Hero
        s2.trees.simTier.back() = static_cast<u8>(SimTier::Impostor); // cold start
    }
    SimLodConfig budget1; budget1.promotionBudget = 1;
    AssignSimTiers(s2, cam, budget1);
    u32 hero = 0;
    for (u32 i = 0; i < s2.trees.Count(); ++i)
        if (s2.trees.simTier[i] == static_cast<u8>(SimTier::Hero)) ++hero;
    check(hero == 1, "promotion budget caps promotions per call to 1");

    // Demotion is immediate (not budgeted): move the camera far, one call demotes all.
    AssignSimTiers(store, glm::vec3(1000.0f, 0.0f, 0.0f), cfg);
    bool allImpostor = true;
    for (u32 i = 0; i < store.trees.Count(); ++i)
        if (store.trees.simTier[i] != static_cast<u8>(SimTier::Impostor)) allImpostor = false;
    check(allImpostor, "moving the camera far demotes all trees immediately");

    if (fail == 0) HBE_INFO("veglod: all checks passed");
    return fail == 0;
}

} // namespace hbe::veg
