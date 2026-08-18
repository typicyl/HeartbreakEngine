// Vegetation/TreeMesher.h - turns a PlantSkeleton into renderable geometry.
//
// The mesher is the bridge from the STRUCTURAL representation to triangles: woody nodes
// (trunk/branch/twig) become generalized-cylinder tubes; leaf-cluster nodes become
// crossed-quad cards. It emits a Model (a woody submesh + a foliage submesh) so the two
// can carry different materials and be optimized/LOD'd independently through the existing
// meshoptimizer path (mesh::OptimizeForGpu / mesh::BuildLodChain). Deterministic: the
// same skeleton always produces byte-identical geometry.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Vegetation/Species.h"
#include "Assets/Mesh.h"

namespace hbe::veg {

struct TreeMeshSettings {
    u32  ringSegments = 6;   // vertices around a branch tube (higher = rounder, costlier)
    bool foliage = true;     // emit leaf-cluster cards
    f32  leafScale = 1.0f;   // multiplies Species::leafSize
    bool optimize = true;    // run mesh::OptimizeForGpu on each submesh
    bool buildLods = true;   // run mesh::BuildLodChain on each submesh
};

// Build a Model from a skeleton. Submesh [0] = woody (bark material), [1] = foliage (leaf
// material) when there are any leaf clusters and settings.foliage is set. Returns an empty
// Model for an empty skeleton.
Model BuildTreeMesh(const PlantSkeleton& skel, const Species& sp,
                    const TreeMeshSettings& s = {});

} // namespace hbe::veg
