// Assets/MeshDerive.h - deriving normals and tangents for geometry nobody authored.
//
// An imported mesh arrives with normals and tangents already in it. GENERATED geometry does
// not: a surface solved from an anatomical field produces positions and connectivity, and
// everything the shader needs beyond that has to be computed. This is that step.
//
// THE PART THAT IS EASY TO GET WRONG IS HANDEDNESS. `Vertex::tangent.w` carries the sign
// that tells the shader which way the bitangent points (`cross(N, T) * w`). Whenever a UV
// chart is MIRRORED - which every human template does, because the left and right halves of
// a body share one texture island to halve the texture budget - the sign flips. Get it wrong
// and normal maps light correctly on one side of the body and inside-out on the other, which
// reads as "the normal map is broken" rather than "the tangents are wrong" and costs a day.
#pragma once

#include "Assets/Mesh.h"

namespace hbe::mesh {

// Area-weighted vertex normals from the triangles. Area weighting rather than a plain
// average because an equal average lets a fan of slivers outvote one large face, which
// bends the normal toward whichever region happens to be more finely tessellated - visible
// as shading that changes when a mesh is re-meshed at a different density.
void RecomputeNormals(MeshData& mesh);

// Per-vertex tangent frames from UV derivatives, with the handedness sign in `tangent.w`.
// Gram-Schmidt orthogonalised against the normal, so the frame stays orthonormal even where
// the UV mapping is skewed.
//
// Degenerate cases are handled explicitly rather than left to produce NaNs: a triangle with
// zero UV area (a collapsed chart, or geometry with no UVs at all) contributes nothing, and
// a vertex that ends up with no contribution at all gets an arbitrary but VALID frame
// perpendicular to its normal. A NaN tangent silently poisons an entire draw.
void RecomputeTangents(MeshData& mesh);

// Both, in the order that matters - tangents are orthogonalised against the normals, so the
// normals have to be right first.
inline void RecomputeNormalsTangents(MeshData& mesh) {
    RecomputeNormals(mesh);
    RecomputeTangents(mesh);
}

bool SelfTest(); // --test-meshderive

} // namespace hbe::mesh
