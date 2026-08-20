// Assets/MeshCsg.h - Constructive Solid Geometry for the BLOCKOUT BOX BRUSH.
//
// This is the geometry engine behind the Unreal-style box brush: an editable box PRIMITIVE that
// becomes real level geometry, and SUBTRACTIVE boxes that boolean-carve doorways / rooms / windows
// out of the solid. It is NOT a decal or a material projection - it produces actual triangulated
// MeshData that is rendered, collided against, and baked into the navmesh like any other mesh.
//
// The solver is a classic BSP CSG (Naylor-Thibault; the algorithm popularised by csg.js): each box
// becomes 6 planar polygons, the polygons are clipped against each other's BSP trees, and the
// difference is re-triangulated. Boxes are convex, so the trees stay tiny and the result is robust.
//
// Backend-agnostic and headless (no RHI / Renderer / Scene): pure geometry in / MeshData out, so it
// is fully unit-testable (`--test-csg`). The editor owns placement, GPU upload, collision and nav.
#pragma once

#include "Assets/Mesh.h" // hbe::MeshData (the output)
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::csg {

// How a brush combines with the solid it belongs to.
enum class Op : u8 {
    Add = 0,      // adds solid (a wall, a floor, a pillar)
    Subtract = 1, // carves solid away (a doorway, a window, a hollow room interior)
};

// An oriented box: `transform` maps the canonical box spanning [-half, +half] into the working
// space (world, or a body brush's local frame). Rotation + non-uniform scale are honoured because
// every face plane is recomputed from the transformed corners.
struct Box {
    glm::mat4 transform{1.0f};
    glm::vec3 half{1.0f};
};

// Build the triangulated solid = `body` box MINUS every box in `cutters`, all expressed in the SAME
// space, and return it as a single MeshData in that space. Faces are flat-shaded (per-face normal)
// with world-planar tiling UVs (`uvScale` = metres per texture tile) so a grid/checker blockout
// material tiles uniformly across the whole shell. Empty `cutters` returns the plain body box.
// Deterministic: identical inputs -> byte-identical output.
MeshData CarveBox(const Box& body, const std::vector<Box>& cutters, f32 uvScale = 2.0f);

// True when working-space point `p` is inside the carved solid (inside `body` AND outside every
// `cutter`). Exact box tests, independent of the BSP result - the oracle the CSG tests check against.
bool PointSolid(const glm::vec3& p, const Box& body, const std::vector<Box>& cutters);

// Headless correctness suite (`--test-csg`): closed-mesh signed volume of plain / interior-void /
// doorway carves against the analytic answer, PointSolid agreement, winding (positive volume), and
// determinism. Returns true iff every block passed.
bool SelfTest();

} // namespace hbe::csg
