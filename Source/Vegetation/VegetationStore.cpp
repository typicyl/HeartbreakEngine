// Vegetation/VegetationStore.cpp - per-shard SoA store operations.
#include "Vegetation/VegetationStore.h"

namespace hbe::veg {

TreeId VegetationStore::AddTree(const glm::mat4& xform, SpeciesId species, u64 seed,
                                const PlantSkeleton& skel, f32 age) {
    const u32 branchBegin = branches.Count();

    // Flatten the skeleton's node graph into branch SEGMENTS (parent-node -> node) and
    // leaf-cluster ORIGINS. A node whose parent is a real node contributes one segment;
    // a LeafCluster node contributes one foliage cluster. Roots (parent == -1) anchor
    // the graph but emit no segment of their own.
    const u32 n = skel.NodeCount();
    for (u32 i = 0; i < n; ++i) {
        const PlantPart kind = static_cast<PlantPart>(skel.kind[i]);
        const i32 par = skel.parent[i];

        if (kind == PlantPart::LeafCluster || kind == PlantPart::Leaf ||
            kind == PlantPart::Flower || kind == PlantPart::Fruit) {
            const glm::vec3 nrm =
                (par >= 0) ? glm::normalize(skel.pos[i] - skel.pos[par] +
                                            glm::vec3(0.0f, 1e-4f, 0.0f))
                           : glm::vec3(0.0f, 1.0f, 0.0f);
            clusters.origin.push_back(skel.pos[i]);
            clusters.normal.push_back(nrm);
            clusters.density.push_back(1);
            clusters.atlasSlice.push_back(0);
            continue;
        }

        if (par >= 0) {
            branches.a.push_back(skel.pos[par]);
            branches.b.push_back(skel.pos[i]);
            branches.radiusA.push_back(skel.radius[par]);
            branches.radiusB.push_back(skel.radius[i]);
            branches.parent.push_back(-1); // branch-local graph rebuilt in the mesher (P3)
            branches.order.push_back(skel.order[i]);
            branches.windPhase.push_back(0.0f);
        }
    }

    const u32 branchCount = branches.Count() - branchBegin;
    const TreeId id{ trees.Count() };
    trees.xform.push_back(xform);
    trees.species.push_back(species);
    trees.seed.push_back(seed);
    trees.age.push_back(age);
    trees.health.push_back(1.0f);
    trees.simTier.push_back(static_cast<u8>(SimTier::Far));
    trees.branchBegin.push_back(branchBegin);
    trees.branchCount.push_back(branchCount);
    trees.mesh.push_back(rhi::MeshHandle{});
    return id;
}

void VegetationStore::Clear() {
    trees.xform.clear(); trees.species.clear(); trees.seed.clear();
    trees.age.clear(); trees.health.clear(); trees.simTier.clear();
    trees.branchBegin.clear(); trees.branchCount.clear(); trees.mesh.clear();
    branches.a.clear(); branches.b.clear();
    branches.radiusA.clear(); branches.radiusB.clear();
    branches.parent.clear(); branches.order.clear(); branches.windPhase.clear();
    clusters.origin.clear(); clusters.normal.clear();
    clusters.density.clear(); clusters.atlasSlice.clear();
    instanceBuffer = rhi::GpuBufferHandle{};
}

usize VegetationStore::ApproxBytes() const {
    usize b = 0;
    b += trees.xform.size() * sizeof(glm::mat4);
    b += trees.species.size() * sizeof(SpeciesId);
    b += trees.seed.size() * sizeof(u64);
    b += trees.age.size() * sizeof(f32);
    b += trees.health.size() * sizeof(f32);
    b += trees.simTier.size() * sizeof(u8);
    b += trees.branchBegin.size() * sizeof(u32);
    b += trees.branchCount.size() * sizeof(u32);
    b += trees.mesh.size() * sizeof(rhi::MeshHandle);
    b += branches.a.size() * sizeof(glm::vec3) * 2;
    b += branches.radiusA.size() * sizeof(f32) * 2;
    b += branches.parent.size() * sizeof(i32);
    b += branches.order.size() * sizeof(u8);
    b += branches.windPhase.size() * sizeof(f32);
    b += clusters.origin.size() * sizeof(glm::vec3);
    b += clusters.normal.size() * sizeof(glm::vec3);
    b += clusters.density.size() * sizeof(u16);
    b += clusters.atlasSlice.size() * sizeof(u16);
    return b;
}

} // namespace hbe::veg
