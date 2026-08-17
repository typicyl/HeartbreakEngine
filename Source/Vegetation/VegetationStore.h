// Vegetation/VegetationStore.h - the per-shard, data-oriented side store.
//
// ONE store per resident streamed shard. Bulk vegetation data lives HERE as packed SoA
// keyed by typed handles, NEVER as entt entities and never as one C++ object per
// leaf/branch/blade. That is what makes 10k trees / 1M branches / 10M+ foliage feasible:
//   * trees  -> a few thousand transforms + slices,
//   * branches -> a flat SoA sliced per tree,
//   * foliage -> a few thousand CLUSTER params; the 10M leaf/grass instances are
//                GPU-expanded per frame from those params (P5), never materialized here.
// See docs/Design-Vegetation.md section 3.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Core/Types.h"
#include "RHI/RHI.h" // rhi::MeshHandle / rhi::GpuBufferHandle (shared/owned GPU handles)

#include <glm/glm.hpp>

#include <vector>

namespace hbe::veg {

struct VegetationStore {
    // --- Trees (thousands) : one row per plant instance ---
    struct Trees {
        std::vector<glm::mat4> xform;    // world transform
        std::vector<SpeciesId> species;
        std::vector<u64>       seed;     // deterministic re-generation key
        std::vector<f32>       age;      // [0,1] of lifespan
        std::vector<f32>       health;   // 1 = healthy .. 0 = dead
        std::vector<u8>        simTier;  // SimTier
        std::vector<u32>       branchBegin; // [begin,begin+count) slice into Branches SoA
        std::vector<u32>       branchCount;
        // Mesh is SHARED per (species, LOD) through the scene mesh cache - NOT owned per
        // instance, so thousands of trees reference one GPU mesh (P3/P6).
        std::vector<rhi::MeshHandle> mesh;
        u32 Count() const { return static_cast<u32>(xform.size()); }
    } trees;

    // --- Branches (up to ~1M) : flat SoA, sliced per tree via Trees::branchBegin/Count.
    struct Branches {
        std::vector<glm::vec3> a, b;          // segment endpoints, plant-local
        std::vector<f32> radiusA, radiusB;    // radii at each end
        std::vector<i32> parent;              // branch-local parent index (support/break)
        std::vector<u8>  order;               // branch order
        std::vector<f32> windPhase;           // per-branch wind phase offset
        u32 Count() const { return static_cast<u32>(a.size()); }
    } branches;

    // --- Foliage clusters : compact params only. The 10M leaves/blades are GPU-expanded.
    struct FoliageClusters {
        std::vector<glm::vec3> origin;   // cluster origin, plant-local
        std::vector<glm::vec3> normal;   // surface/growth normal (leaf orientation seed)
        std::vector<u16> density;        // leaves/blades this cluster expands to
        std::vector<u16> atlasSlice;     // leaf/species texture-atlas slice
        u32 Count() const { return static_cast<u32>(origin.size()); }
    } clusters;

    // GPU per-instance buffer (compute-expanded per frame, created in P5). The store
    // does not own it in P1; the eviction path releases it via the streamer (P8).
    rhi::GpuBufferHandle instanceBuffer{};

    // Adds a tree instance built from a generated skeleton: appends the tree row and
    // flattens the skeleton's branch/twig nodes into the Branches SoA and its
    // leaf-cluster nodes into FoliageClusters. Returns the new tree's handle.
    TreeId AddTree(const glm::mat4& xform, SpeciesId species, u64 seed,
                   const PlantSkeleton& skel, f32 age = 1.0f);

    void Clear();

    // Rough resident-CPU footprint (for the streaming/VRAM budget reporting in P8).
    usize ApproxBytes() const;

    bool Empty() const {
        return trees.Count() == 0 && branches.Count() == 0 && clusters.Count() == 0;
    }
};

} // namespace hbe::veg
