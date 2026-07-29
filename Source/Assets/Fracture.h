// Assets/Fracture.h - Voronoi mesh fracture + the .hbfrac destructible asset.
//
// Fracturing is done OFFLINE in the editor, never at runtime: the geometry work
// is far too expensive to do on the frame a wall gets shot, and doing it ahead of
// time also makes destruction deterministic (the same wall always breaks into the
// same pieces), which is what lets a save file and a network replay agree.
//
// ALGORITHM (Voronoi cell decomposition by half-space clipping):
//   1. Scatter N sites inside the source mesh's bounds using the chosen pattern.
//   2. A Voronoi cell for site i is, by definition, every point closer to i than
//      to any other site - i.e. the intersection of the half-spaces bounded by
//      the perpendicular bisector plane between i and each other site. That
//      intersection is always CONVEX, which is the property everything else here
//      leans on (convex chunks get exact ConvexHullShape colliders for free, and
//      the engine's physics already supports that shape).
//   3. Start from the source AABB as a convex polyhedron and clip it by each
//      bisector. Clipping a convex polyhedron by a plane = Sutherland-Hodgman on
//      every face, plus ONE new capping face built from the cut edges - that cap
//      is the interior "broken" surface and gets the interior material.
//   4. Faces that survive clipping against a *neighbour's* bisector mean the two
//      cells touch, which yields the adjacency graph the structural-integrity
//      solver walks.
//
// This is an approximation for CONCAVE source meshes (the cells are clipped
// against the bounding volume, not the exact surface). That is the standard
// trade-off for game fracture and is exact for the box/slab/column shapes that
// make up most breakable set dressing.
#pragma once

#include "Assets/Mesh.h"
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

// How fracture sites are distributed. The pattern is what makes a break read as
// the right KIND of break: a bullet is radial, a dropped crate is uniform.
enum class FracturePattern : u8 {
    Uniform = 0, // evenly scattered - generic shatter
    Clustered,   // tight groups - stone/concrete (a few big pieces, some rubble)
    Radial,      // dense at an impact point, sparse outward - bullets, blasts
    Slabs,       // biased along one axis - planks, panels, glass sheets
};

const char* FracturePatternName(FracturePattern p);

struct FractureSettings {
    FracturePattern pattern = FracturePattern::Uniform;
    u32 cellCount = 24;          // target chunk count
    u32 seed = 1337;             // deterministic: same seed = same break
    glm::vec3 impactPoint{0.0f}; // Radial: centre (object space)
    f32 radialFalloff = 2.0f;    // Radial: >1 concentrates harder at the centre
    glm::vec3 slabAxis{0, 1, 0}; // Slabs: the axis cells are flattened along
    f32 slabBias = 4.0f;         // Slabs: how strongly flattened
    // Chunks smaller than this fraction of the mean chunk volume are merged away.
    // Voronoi naturally produces slivers at the hull; they are worthless as debris
    // and each one still costs a body, a draw and a collider.
    f32 minChunkVolumeFrac = 0.05f;
};

// One fractured piece. Always convex, so its collider is an exact ConvexHullShape.
struct FractureChunk {
    MeshData mesh;              // chunk geometry, in the SOURCE mesh's object space
    glm::vec3 centroid{0.0f};   // object-space centre of mass
    f32 volume = 0.0f;          // -> mass at runtime (volume * density)
    std::vector<u32> neighbours; // indices of chunks sharing a cut face
    // True when the chunk touches the anchor region (see FractureSettings usage in
    // the editor: typically the base of the object). Anchored chunks are the roots
    // the structural-integrity flood fill starts from - everything that loses its
    // path to an anchor falls.
    bool anchored = false;
};

struct FractureAsset {
    std::vector<FractureChunk> chunks;
    // Material applied to the NEWLY CUT interior faces. Without this a broken
    // concrete wall shows its painted exterior on the inside, which instantly
    // reads as fake.
    std::string interiorMaterial;
    glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
    FractureSettings settings; // what produced this (for re-fracturing in the editor)
};

namespace assets {

inline constexpr const char* kFractureExtension = ".hbfrac";

// Fractures `source` into chunks. Pure and deterministic for a given seed, so it
// is safe to call from a job. Returns nullopt when the source is degenerate.
std::optional<FractureAsset> FractureMesh(const MeshData& source,
                                          const FractureSettings& settings);

bool SaveFracture(const std::filesystem::path& path, const FractureAsset& a);
// Pack-aware (VFS), like every other runtime asset load.
std::optional<FractureAsset> LoadFracture(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe
