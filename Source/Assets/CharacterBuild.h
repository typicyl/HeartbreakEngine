// Assets/CharacterBuild.h - assemble + seam-weld a modular character.
//
// Loads the canonical skeleton and every variant's part mesh referenced by a
// CharacterAsset, remaps each part onto the canonical skeleton by bone name, and
// welds all seams across ALL variants (so any loadout combination is solid). The
// result is a set of welded, GPU-ready part meshes keyed by variant id. Pure CPU
// and deterministic, so the same inputs always produce identical welded buffers
// (safe to run at editor-build time or at runtime instantiation).
#pragma once

#include "Assets/Animation.h" // Skeleton
#include "Assets/Mesh.h"      // MeshData
#include "Assets/SeamWeld.h"  // weld::Stats
#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace hbe {

struct CharacterAsset;

namespace assets {

struct BuiltCharacter {
    bool ok = false;
    Skeleton skeleton;             // canonical skeleton (drives every part)
    std::string skeletonSource;    // "uaf:<rel>#<n>" for the root's MeshRef/Animator
    // variant id -> welded + remapped part mesh, ready to upload.
    std::unordered_map<std::string, MeshData> welded;
    weld::Stats stats;
    // Rest-pose positions of open-boundary verts (no cross-part partner) - drift
    // risks the editor overlay markers.
    std::vector<glm::vec3> openBoundaryPositions;
};

// Builds `c` against the project's `assetsDir`. Returns ok=false if the skeleton
// or all variant meshes fail to load.
BuiltCharacter BuildCharacter(const std::filesystem::path& assetsDir, const CharacterAsset& c);

} // namespace assets
} // namespace hbe
