// Assets/SeamWeld.h - the "solid seam" guarantee for modular characters.
//
// Skinning is deterministic: a vertex's final position is a fixed 4-term blend
// Σ wᵢ·palette[jᵢ]·restPosᵢ (see Shaders/MeshPBR.hlsl SkinMatrix). Two vertices
// from DIFFERENT parts land at bit-identical positions every frame iff they carry
// identical rest position + identical (joint,weight) bindings AND read the same
// palette. All parts of a character share ONE palette, so seam solidity reduces to
// making the connecting boundary vertices carry identical bindings.
//
// WeldSeams does exactly that at BUILD time: it finds each part's open geometric
// boundary, groups boundary vertices that coincide across parts, and stamps ONE
// canonical binding (position + joints + weights, and normal/tangent for smooth
// seams) into every member. Because it groups per boundary LOCATION across all
// variants of all slots, any loadout combination is automatically gap-free.
#pragma once

#include "Assets/CharacterAsset.h" // SeamMode
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe {

struct MeshData; // Assets/Mesh.h
struct Skeleton; // Assets/Animation.h

namespace weld {

// One part participating in a weld. `mesh` is mutated in place.
struct Part {
    MeshData* mesh = nullptr;
    SeamMode seamMode = SeamMode::Continuous;
    bool isMaster = false; // canonical binding source (the base body) for shared seams
};

struct Stats {
    u32 boundaryVertices = 0;  // total open-boundary verts across all parts
    u32 groups = 0;            // cross-part seam groups (>=2 distinct parts) welded
    u32 weldedVertices = 0;    // vertices rewritten to a canonical binding
    u32 openBoundary = 0;      // boundary verts with NO cross-part partner (hem, or drift risk)
    u32 nonManifoldEdges = 0;  // edges shared by >2 triangles (authoring warning)
    f32 tolerance = 0.0f;      // grouping radius used (world/model units)
};

// Builds a table srcJoint -> canonical joint index by matching bone NAMES (with the
// same DCC canonicalization the animation system uses). Unmatched -> -1.
std::vector<i32> BuildJointRemap(const Skeleton& src, const Skeleton& canon);

// Rewrites every vertex's joint indices in `mesh` from a source skeleton's index
// space into the canonical skeleton's via `srcToCanon` (whole mesh - all vertices
// must index the shared palette). An unmatched influence (-1) is dropped (its
// weight zeroed) and the vertex renormalized. No-op mapping (identity) leaves the
// mesh bit-unchanged.
void RemapJoints(MeshData& mesh, const std::vector<i32>& srcToCanon);

// Open-boundary vertices of a mesh in POSITION topology (UV/normal duplicates at
// the same position are merged first, so they are NOT treated as boundaries).
// Returns LOCAL vertex indices. Used by the editor seam-validation overlay.
std::vector<u32> OpenBoundaryVertices(const MeshData& mesh, f32 mergeEps = 1e-5f);

// Canonicalizes seam vertices across `parts` so adjacent parts skin bit-identically
// at the boundary. `toleranceScale` * (combined bbox diagonal) = the grouping
// radius. Mutates each part's MeshData. Parts must already be remapped onto ONE
// skeleton (call RemapJoints first). Returns stats (openBoundary feeds the overlay).
// `outOpenPositions` (optional) collects the rest-pose positions of boundary
// vertices with NO cross-part partner - the editor overlay markers these as drift
// risks (or legitimate open hems the artist can dismiss).
Stats WeldSeams(std::vector<Part>& parts, f32 toleranceScale = 1e-4f,
                std::vector<glm::vec3>* outOpenPositions = nullptr);

// Self-contained, GPU-free proof of the guarantee: two parts with a shared seam but
// DIFFERENT authored bindings -> weld -> assert the seam vertices skin to
// bit-identical positions under a palette. Returns true on pass (logs details).
bool SelfTest();

} // namespace weld
} // namespace hbe
