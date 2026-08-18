// Vegetation/VegetationSystem.cpp - per-frame tick + skeleton hashing.
#include "Vegetation/VegetationSystem.h"
#include "Vegetation/VegetationWorld.h"
#include "Core/Rng.h"

namespace hbe::veg {

u64 HashSkeleton(const PlantSkeleton& s) {
    // Fixed-order, bit-pattern hash (never container iteration order, never a float
    // compare) so it means "the same structure" exactly and is stable across compilers.
    Hasher h;
    h.Mix(s.sourceSeed);
    h.Mix(static_cast<u32>(s.NodeCount()));
    const u32 n = s.NodeCount();
    for (u32 i = 0; i < n; ++i) {
        h.Mix(s.pos[i].x); h.Mix(s.pos[i].y); h.Mix(s.pos[i].z);
        h.Mix(s.parent[i]);
        h.Mix(s.radius[i]);
        h.Mix(static_cast<u32>(s.order[i]));
        h.Mix(s.age[i]);
        h.Mix(static_cast<u32>(s.kind[i]));
    }
    return h.Value();
}

void Update(VegetationWorld& world, Scene& scene, const glm::vec3& camPos, f32 dt,
            bool simulating) {
    (void)scene;
    (void)dt;
    // Simulation-LOD runs every frame (presentational): tier each resident shard's trees by
    // camera distance. Growth/structural change (gated on `simulating`) lands in P10-full;
    // it would go behind the same walk. Cheap when nothing is resident.
    (void)simulating;
    world.ForEachStore([&](u64, VegetationStore& store) { AssignSimTiers(store, camPos); });
}

} // namespace hbe::veg
