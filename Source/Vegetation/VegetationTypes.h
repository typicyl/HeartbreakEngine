// Vegetation/VegetationTypes.h - foundational vegetation value types.
//
// The vegetation subsystem is data-oriented: a plant is NOT "Transform + Mesh". Its
// structure is an SoA node graph (PlantSkeleton) shared by every generator, the
// mesher, wind, LOD, growth and damage. A leaf is an INDEX into a packed array, never
// a heavyweight C++ object - which is what lets the engine represent millions of
// structural elements cheaply while still being able to address any one of them.
//
// See docs/Design-Vegetation.md for the whole architecture.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::veg {

// --- Interned asset identities (data-driven; see SpeciesRegistry / BiomeRegistry) ---
// A species/biome is authored in a .hbspecies/.hbbiome asset and interned to a stable
// small integer at load; everything downstream keys off the id, never a string.
struct SpeciesId {
    u32 v = 0xFFFFFFFFu;
    bool Valid() const { return v != 0xFFFFFFFFu; }
    bool operator==(const SpeciesId&) const = default;
};
struct BiomeId {
    u32 v = 0xFFFFFFFFu;
    bool Valid() const { return v != 0xFFFFFFFFu; }
    bool operator==(const BiomeId&) const = default;
};

// --- Runtime handles into the per-shard VegetationStore SoA -------------------------
// Typed u32 indices, not pointers: the backing std::vectors may reallocate, and a
// handle survives that where a pointer would dangle. 0xFFFFFFFF = invalid.
struct TreeId {
    u32 v = 0xFFFFFFFFu;
    bool Valid() const { return v != 0xFFFFFFFFu; }
    bool operator==(const TreeId&) const = default;
};
struct BranchId {
    u32 v = 0xFFFFFFFFu;
    bool Valid() const { return v != 0xFFFFFFFFu; }
    bool operator==(const BranchId&) const = default;
};
struct FoliageClusterId {
    u32 v = 0xFFFFFFFFu;
    bool Valid() const { return v != 0xFFFFFFFFu; }
    bool operator==(const FoliageClusterId&) const = default;
};

// Structural role of a skeleton node. Kept as a small enum so a node costs one byte.
enum class PlantPart : u8 {
    Trunk = 0,
    Branch,
    Twig,
    LeafCluster,
    Leaf,
    Flower,
    Fruit,
    Root,
    Count
};

// Simulation detail tier - INDEPENDENT of render LOD. Hero can run per-leaf-cluster
// physics; Impostor does no CPU sim at all. The simulation LOD system (P7) promotes
// and demotes instances between tiers under a fixed budget so a camera sweep never
// spikes. See docs/Design-Vegetation.md section 4.
enum class SimTier : u8 {
    Hero = 0,   // per-leaf-cluster / per-branch, highest frequency
    Near,       // per-branch wind + trunk sway
    Mid,        // trunk + primary-branch sway; leaves are shader flutter
    Far,        // trunk bend only
    Impostor,   // no CPU sim; octahedral impostor + UV-drift wind
    Count
};

// ===== The crown-jewel structural representation ====================================
// An SoA node graph. EVERY generator (space colonization, L-system, custom, external)
// emits this, and the mesher, wind, growth and damage all consume it. Nothing keys off
// triangles. Storing millions of nodes is a handful of contiguous vectors, not a graph
// of allocations.
struct PlantSkeleton {
    std::vector<glm::vec3> pos;    // node position, plant-LOCAL space
    std::vector<i32>       parent; // parent node index; -1 = root
    std::vector<f32>       radius; // node radius (m) - branch thickness at this node
    std::vector<u8>        order;  // branch order: 0 trunk .. N twig
    std::vector<f32>       age;    // node age in [0,1] of the plant's life (for growth)
    std::vector<u8>        kind;   // PlantPart (stored as u8)
    u64 sourceSeed = 0;            // the stable seed this skeleton was grown from:
                                   // deterministic re-generation + mesh-cache key

    u32 NodeCount() const { return static_cast<u32>(pos.size()); }

    void Reserve(u32 n) {
        pos.reserve(n); parent.reserve(n); radius.reserve(n);
        order.reserve(n); age.reserve(n); kind.reserve(n);
    }
    void Clear() {
        pos.clear(); parent.clear(); radius.clear();
        order.clear(); age.clear(); kind.clear();
        sourceSeed = 0;
    }

    // Appends a node and returns its index. THE ONE place the parallel arrays advance
    // together - every other writer goes through here, so they can never fall out of
    // lockstep (the invariant the self-test pins).
    u32 AddNode(const glm::vec3& p, i32 par, f32 r, u8 ord, PlantPart k, f32 a = 1.0f) {
        const u32 idx = NodeCount();
        pos.push_back(p);
        parent.push_back(par);
        radius.push_back(r);
        order.push_back(ord);
        age.push_back(a);
        kind.push_back(static_cast<u8>(k));
        return idx;
    }

    // All arrays equal length, every parent index in range (or -1), and node 0 (if any)
    // is a root. Cheap; the generation self-test runs it on every produced skeleton.
    bool Validate() const {
        const usize n = pos.size();
        if (parent.size() != n || radius.size() != n || order.size() != n ||
            age.size() != n || kind.size() != n)
            return false;
        for (usize i = 0; i < n; ++i) {
            const i32 par = parent[i];
            if (par < -1 || par >= static_cast<i32>(n)) return false;
            if (par == static_cast<i32>(i)) return false; // no self-parent
        }
        return true;
    }
};

// Deterministic content hash of a skeleton, over raw bit patterns in a fixed order
// (never container iteration order, never a float compare). Used as the bake-cache key
// and by the determinism self-test ("same seed -> same bytes"). Implemented in
// VegetationSystem.cpp so the header stays dependency-light.
u64 HashSkeleton(const PlantSkeleton& s);

} // namespace hbe::veg
