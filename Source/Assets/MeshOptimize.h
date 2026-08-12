// Assets/MeshOptimize.h - import-time GPU geometry optimization (zeux/meshoptimizer).
//
// WHY. An imported mesh's triangle and vertex ORDER is whatever the DCC tool and the
// importer happened to emit. The GPU does not care what the mesh looks like, but it
// cares a great deal about that order: the post-transform vertex cache only reuses a
// recently shaded vertex if the triangles sharing it are drawn close together, and the
// pre-transform fetch cost drops when vertices referenced near each other in the index
// buffer also live near each other in the vertex buffer. Neither happens by accident.
// meshoptimizer reorders both, deterministically, ONCE at import - a pure runtime win
// that costs nothing at draw time and changes no bytes the shader reads (only the ORDER
// of the same vertices/indices). The engine previously did none of this beyond Assimp's
// aiProcess_ImproveCacheLocality; this owns the ordering end to end.
//
// THE ONE HAZARD is morph targets. MorphTarget::posDelta / nrmDelta are PARALLEL to
// MeshData::vertices (indexed by vertex id). Any pass that reorders or resizes the vertex
// array must apply the SAME remap to every morph target, or blendshapes silently drift
// off their vertices. OptimizeForGpu does this in lockstep - that is the entire reason it
// is a dedicated function and not a handful of inline meshopt calls at the import site.
// It also SKIPS the vertex-weld step on morph-bearing meshes, because welding two coincident
// base vertices that carry different deltas would corrupt the blendshape.
#pragma once

#include "Assets/Mesh.h"

namespace hbe::mesh {

struct OptimizeStats {
    u32 inputVertices = 0;
    u32 outputVertices = 0;   // after welding binary-identical / dropping unused vertices
    u32 triangles = 0;
    f32 acmrBefore = 0.0f;    // avg cache miss ratio (16-entry cache), before / after
    f32 acmrAfter = 0.0f;
};

// Reorders one submesh's vertices and indices for GPU cache / overdraw / fetch efficiency
// (weld -> vertex-cache -> overdraw -> vertex-fetch). Rewrites md.vertices and md.indices
// in place and carries every morph target's delta arrays through the same vertex remap so
// blendshapes stay aligned. Deterministic. A no-op on an empty mesh. Fills `out` when given.
void OptimizeForGpu(MeshData& md, OptimizeStats* out = nullptr);

bool OptimizeSelfTest(); // --test-meshopt

} // namespace hbe::mesh
