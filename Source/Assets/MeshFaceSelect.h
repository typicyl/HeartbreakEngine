// Assets/MeshFaceSelect.h - picking triangles on a mesh, and giving them their own material.
//
// HOW A SECOND MATERIAL ACTUALLY WORKS HERE. This engine already represents a multi-material
// model exactly one way: a submesh IS a MeshData with ONE Material, the importer writes one
// .hbmat per material, and spawning walks the submeshes. So "assign a different material to
// these faces" is not a new rendering concept at all - it is a mesh operation. Split the
// chosen triangles into a new submesh and point it at another material, and every existing
// system (draw submission, .hbmat editing, packing, the asset viewer) works unchanged.
//
// THE CONSTRAINT THAT SHAPES THE WHOLE DESIGN: scenes reference submeshes positionally, as
// "uaf:<path>#<index>". Renumbering submeshes silently repoints every entity in every scene
// at different geometry. So a split NEVER reorders: the original submesh keeps its index and
// its remaining faces, and the extracted group is APPENDED at the end. Existing references
// stay correct by construction rather than by a migration pass.
#pragma once

#include "Assets/Mesh.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::mesh {

// Ray/triangle intersection over a whole mesh, in the mesh's own space. Returns the index of
// the nearest hit TRIANGLE (not vertex), or -1. `outT` receives the distance along the ray.
//
// Deliberately brute force. A picking ray is cast on a click, not per frame, and an
// acceleration structure here would be a second source of truth to keep in sync with an
// actively edited mesh - the wrong trade for an authoring path.
i32 RaycastTriangle(const MeshData& mesh, const glm::vec3& origin, const glm::vec3& dir,
                    f32* outT = nullptr);

// Every triangle reachable from `seed` without crossing an edge whose dihedral angle exceeds
// `maxAngleDegrees`. This is the "select the flat panel I clicked on" operation - selecting a
// cube face, a wall, or a window frame by hand is otherwise unusable on real geometry.
std::vector<u32> SelectConnected(const MeshData& mesh, u32 seed, f32 maxAngleDegrees);

// Every triangle whose facing is within `maxAngleDegrees` of the seed's, connected or not.
// The "select all the upward faces" operation.
std::vector<u32> SelectSimilarFacing(const MeshData& mesh, u32 seed, f32 maxAngleDegrees);

struct SplitResult {
    MeshData remainder; // keeps the ORIGINAL material; may be empty if everything was taken
    MeshData extracted; // the selected faces, ready to be given another material
    bool tookEverything = false; // caller should just swap the material instead of splitting
};

// Split `src` by triangle index. Vertices used by both halves are DUPLICATED - each submesh
// owns its own vertex buffer, so a shared vertex has to exist in both. Normals and tangents
// are carried over unchanged rather than recomputed: the geometry has not moved, only its
// grouping, and recomputing would round the shading at the new border.
SplitResult SplitByFaces(const MeshData& src, const std::vector<u32>& faces);

bool FaceSelectSelfTest(); // --test-faceselect

} // namespace hbe::mesh
