// Navigation/NavBaker.h - EDITOR-ONLY navigation baking (Recast).
//
// This is the ONLY place Recast is used, and the ONLY navigation source compiled just
// into hbe_editor (see HeartbreakEngine/CMakeLists.txt: HBE_NAV_BAKE_SOURCES). The
// shipped runtime never links Recast; it consumes the `.hbnav` this produces through
// Detour + DetourTileCache alone.
//
// The baker gathers ALL navigation-input geometry from the scene at author time -
// including geometry that is TAGGED for streaming - and voxelises it into a tiled
// DetourTileCache layer set, one column per (x,y) tile. Because the result lives on
// disk in a `.hbnav`, navigation persists independently of which parts of the world are
// visually loaded at runtime.
#pragma once

#include "Core/Types.h"
#include "Navigation/NavFormat.h" // NavAgentProfile

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace hbe {

class Scene;

namespace nav {

// Recast build parameters that actually affect a tiled DetourTileCache bake. (Region /
// detail-mesh settings do not apply to the tile-cache path - runtime tile rebuild owns
// them - so they are deliberately omitted; the tile cache trades the detail mesh for
// dynamic-obstacle support, a standard Recast tradeoff.)
struct NavBuildSettings {
    f32 cellSize = 0.3f;         // xz voxel size (m)
    f32 cellHeight = 0.2f;       // y voxel size (m)
    i32 tileVoxels = 48;         // tile edge in cells; tileWorldSize = tileVoxels*cellSize
    f32 agentRadius = 0.6f;      // eroded from walkable area
    f32 agentHeight = 2.0f;      // vertical clearance to stand
    f32 agentMaxClimb = 0.9f;    // step height an agent can climb
    f32 agentMaxSlopeDeg = 45.0f;// steeper = wall
    std::string profileName = "Human";
};

struct NavBakeResult {
    bool ok = false;
    std::vector<u8> bytes; // the serialised .hbnav (write to disk / pack)
    int tileColumns = 0;
    int totalTris = 0;
    u64 sourceHash = 0;
    std::string message;
};

// Bake from a raw world-space triangle soup. `verts` = 3 floats per vertex, `tris` = 3
// int indices per triangle. `cancel` (optional) is polled between tiles. This is the
// core; the Scene overload gathers geometry then calls it.
NavBakeResult BakeNavMeshFromGeometry(const std::vector<f32>& verts,
                                      const std::vector<i32>& tris,
                                      const NavBuildSettings& settings,
                                      const std::atomic<bool>* cancel = nullptr);

// Gather every navigation-input mesh from the scene (all MeshRef entities, including
// streaming-tagged ones; a mesh opts OUT with a disabled NavmeshInput component) and
// bake a .hbnav byte buffer with a single agent profile.
NavBakeResult BakeNavMesh(const Scene& scene, const std::filesystem::path& assetsDir,
                          const NavBuildSettings& settings,
                          const std::atomic<bool>* cancel = nullptr);

// Compute the stale-bake source hash (geometry + settings) without baking, so the
// editor can tell whether a .hbnav is up to date with the scene.
u64 HashNavInputs(const Scene& scene, const std::filesystem::path& assetsDir,
                  const NavBuildSettings& settings);

// Headless end-to-end self-test: bake synthetic geometry, load the .hbnav, stream every
// tile, path around a baked wall, then add/remove a dynamic obstacle and confirm the
// path reroutes and recovers. Returns true on PASS (logs details). Drives the runtime
// NavMesh/NavWorld path, so it also covers Detour consumption.
bool SelfTest();

} // namespace nav
} // namespace hbe
