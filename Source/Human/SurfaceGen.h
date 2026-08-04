// Human/SurfaceGen.h - turning the anatomical field into actual geometry.
//
// This is where a body stops being mathematics and becomes vertices and indices. It runs
// ONCE per generated human, in an authoring tool, so it is allowed to be expensive - and
// that permission is what makes a genuinely field-derived surface possible at all.
//
// SURFACE NETS, not marching cubes. Both extract an isosurface; the difference is where the
// vertex goes. Marching cubes puts vertices on grid EDGES, which produces long thin slivers
// wherever the surface runs near-parallel to a grid plane - poor triangles for deformation
// and worse for normals. Surface nets puts ONE vertex inside each straddling cell and
// connects neighbours, giving quads that are far more uniform. For a body that will be
// skinned and deformed, triangle quality matters more than exactly reproducing the
// isosurface, so this is the right trade.
//
// STABLE TOPOLOGY. The extraction is a pure function of (field, grid), so identical
// parameters give an identical index buffer. Once generated, that topology is FIXED for the
// character's lifetime - the engine's motion vectors, LODs and materials all require stable
// vertex correspondence, and re-extracting per frame would destroy it. Editing a parameter
// re-runs generation in the TOOL; it does not re-run in the game.
#pragma once

#include "Assets/Mesh.h"
#include "Core/Types.h"
#include "Human/BodyField.h"

#include <vector>

namespace hbe::human {

struct SurfaceSettings {
    // Cells along the body's LONGEST axis. Everything else follows from the bounding box, so
    // one number controls density and it means the same thing for a child and an adult.
    u32 resolution = 128;
    // Newton steps pulling each extracted vertex onto the true zero crossing. Surface nets
    // places a vertex from linear edge interpolation, which is off by a fraction of a cell
    // on curved surfaces; two steps removes almost all of it for a few percent of the cost.
    u32 projectSteps = 2;
    // Tangential relaxation passes. Extraction leaves vertices bunched where the surface
    // meets a cell corner obliquely; relaxing them along the surface evens out edge lengths
    // without moving the surface, which is what keeps texel density uniform later.
    u32 relaxPasses = 3;
    f32 relaxStrength = 0.5f;
};

struct GeneratedSurface {
    MeshData mesh;                    // positions, normals, tangents, UVs, indices
    std::vector<Region> vertexRegion; // parallel to mesh.vertices - the anatomical tag
    std::vector<f32> tissueDepth;     // metres of soft tissue over bone/muscle, per vertex
    glm::vec3 boundsLo{0.0f}, boundsHi{0.0f};
    u32 gridX = 0, gridY = 0, gridZ = 0;
    f32 cellSize = 0.0f;
    // Diagnostics an authoring tool should show rather than hide.
    // TWO DIFFERENT DEFECTS, deliberately counted apart because they are not equally bad.
    // A BOUNDARY edge (used by one triangle) is a HOLE - the body is not closed, and nothing
    // downstream survives that. A PINCHED edge (used by three or more) is the known cost of
    // one-vertex-per-cell dual extraction where the surface passes through a cell twice; the
    // body is still closed, and the standard repair is to split the offending vertex fans.
    // That repair belongs in the export stage and is not written yet - so the count is
    // reported rather than hidden.
    u32 boundaryEdges = 0;
    u32 nonManifoldEdges = 0;
    f64 seconds = 0.0;
};

// Extract the skin. `field` must already be Build()'d.
GeneratedSurface Extract(const BodyField& field, const SurfaceSettings& s);

bool SurfaceSelfTest();

} // namespace hbe::human
