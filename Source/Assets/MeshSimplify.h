// Assets/MeshSimplify.h - reducing a mesh to a fraction of its triangles, at import.
//
// WHY QUADRIC ERROR METRICS. The naive ways of reducing a mesh - dropping every other
// triangle, clustering vertices onto a grid - destroy silhouettes, because they have no idea
// which triangles carry the shape. Garland & Heckbert's quadric metric gives every vertex a
// 4x4 matrix summarising the planes of the faces around it, so the "cost" of collapsing an
// edge is literally the squared distance the surface would move. Collapsing cheapest-first
// removes detail from flat regions and leaves it on curved ones, which is why a decimated
// character keeps its outline while a decimated wall becomes two triangles.
//
// WHAT THIS IS FOR. Streaming and distance. A full-detail mesh is the wrong thing to load for
// something the player can barely see, and this engine has no LOD mechanism at all - so the
// reduced mesh is generated once at import and shipped alongside the original, rather than
// computed at runtime where it would cost more than it saves.
//
// WHAT IT DELIBERATELY DOES NOT DO:
//   * It does not preserve UV seams as separate charts. Vertices are welded by POSITION for
//     the purposes of collapsing, so a seam vertex pair collapses together and the UV is
//     taken from one side. At the reduction ratios this is used for, a slightly stretched
//     seam is invisible; at aggressive ratios it is not, and that is a real limit.
//   * It does not re-pack or re-atlas anything.
//   * It does not touch morph targets. A decimated mesh has different vertices, so per-vertex
//     morph deltas do not survive - the LOD is deliberately emitted WITHOUT them rather than
//     with silently misaligned ones.
#pragma once

#include "Assets/Mesh.h"

namespace hbe::mesh {

struct SimplifySettings {
    // Fraction of the ORIGINAL triangle count to keep. 0.25 = a quarter of the triangles.
    f32 ratio = 0.25f;
    // Never go below this, whatever the ratio says. A 12-triangle box asked for 10% would
    // otherwise collapse to nothing and disappear at distance instead of getting simpler.
    u32 minTriangles = 24;
    // Refuse a collapse that would move the surface further than this, even when the ratio
    // has not been met. Expressed as a fraction of the mesh's bounding-box diagonal, so it
    // means the same thing for a doorknob and for a cathedral.
    f32 maxErrorFraction = 0.02f;
    // Reject any collapse that would flip a triangle. Without this, decimation punches holes
    // and inverts normals in tight regions - visible as black facets.
    bool preventFlips = true;
};

struct SimplifyStats {
    u32 inputTriangles = 0;
    u32 outputTriangles = 0;
    u32 collapses = 0;
    u32 rejectedFlips = 0;
    f32 maxError = 0.0f; // worst surface movement actually accepted, in metres
    bool hitErrorLimit = false; // stopped early because further collapses were too costly
};

// Returns a reduced copy. Normals and tangents are REBUILT from the reduced topology (a
// decimated mesh's old normals describe geometry that no longer exists), and the material,
// name and skin binding are carried over. The input is not modified.
MeshData Simplify(const MeshData& src, const SimplifySettings& s, SimplifyStats* out = nullptr);

bool SimplifySelfTest(); // --test-simplify

} // namespace hbe::mesh
