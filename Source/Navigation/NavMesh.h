// Navigation/NavMesh.h - the Detour-backed runtime navmesh, wrapped so no Detour
// type escapes into the rest of the engine.
//
// NavMeshManager owns a WINDOWED dtNavMesh: only the tile columns near the player are
// resident, added/removed by the streaming layer (NavWorld) as the focus moves. It
// also owns a dtTileCache for dynamic obstacles (per-tile rebuild) and a dtNavMeshQuery
// for pathfinding. Every Detour type (dtNavMesh, dtPolyRef, dtTileCache, ...) is hidden
// behind the Impl pimpl, so gameplay/AI code sees only glm vectors and engine structs -
// exactly the "engine-owned API over Detour" the design calls for.
//
// THREADING: all mutating calls (AddColumn/RemoveColumn/obstacles/Update) touch Detour
// state and are MAIN-THREAD ONLY. The const query calls (FindPath/SamplePoint/
// HasPolyNear) use a single shared dtNavMeshQuery and are likewise expected on the main
// thread (the engine issues them from the sim band). The expensive part - reading a
// tile's bytes off disk - happens on a worker BEFORE AddColumn (see NavWorld).
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace hbe {
namespace nav {

// Sizing for a fresh windowed navmesh. Comes straight from the .hbnav header plus the
// streaming window size (maxResidentColumns), which bounds the polyRef bit budget.
struct NavMeshInitParams {
    f32 cellSize = 0.3f;
    f32 cellHeight = 0.2f;
    i32 tileVoxels = 48;
    f32 tileWorldSize = 0.0f; // tileVoxels * cellSize
    glm::vec3 origin{0.0f};
    f32 agentRadius = 0.6f;
    f32 agentHeight = 2.0f;
    f32 agentMaxClimb = 0.9f;
    i32 maxResidentColumns = 256; // streaming window (columns that can be resident at once)
    i32 maxObstacles = 256;
};

// Opaque, salted obstacle handle. Never a dtObstacleRef in the open.
struct NavObstacleHandle {
    u32 id = 0;
    bool Valid() const { return id != 0; }
};

// Result of a path query. `missingTiles` is the streaming signal: start or goal has no
// resident poly, so the caller should request those tiles and retry - NOT treat it as
// "unreachable".
struct NavPathResult {
    bool found = false;
    bool partial = false;      // reached the closest poly to the goal, not the goal
    bool missingTiles = false; // start/goal not on any resident poly
    std::vector<glm::vec3> corners;
};

class NavMeshManager : public NonCopyable {
public:
    NavMeshManager();
    ~NavMeshManager();

    // Build an empty windowed navmesh + tile cache + query. Returns false on failure
    // (bad params / out of memory); the manager is then !Ready().
    bool Init(const NavMeshInitParams& params);
    void Shutdown();
    bool Ready() const;

    // --- Tile streaming (main thread) ---------------------------------------
    // `blob` is one column's raw .hbnav payload: [u32 layerLen][compressed layer]...
    // Adds every layer to the tile cache and builds the nav tiles at (tx,ty).
    bool AddColumn(i32 tx, i32 ty, const std::vector<u8>& blob);
    void RemoveColumn(i32 tx, i32 ty);
    bool HasColumn(i32 tx, i32 ty) const;
    int ResidentColumnCount() const;
    // World AABB (XZ) of a resident column, for the obstacle-overlap reconciliation.
    bool ColumnBounds(i32 tx, i32 ty, glm::vec3& outMin, glm::vec3& outMax) const;

    // --- Dynamic obstacles (main thread) ------------------------------------
    NavObstacleHandle AddObstacle(const glm::vec3& pos, f32 radius, f32 height);
    void RemoveObstacle(NavObstacleHandle h);
    // Drains queued obstacle add/remove requests, rebuilding affected resident tiles.
    // Detour bounds this internally (64 requests / 64 tiles per call); loops up to a
    // few times so a burst settles quickly without unbounded main-thread cost.
    void UpdateObstacles(f32 dt);
    bool HasPendingObstacleWork() const;

    // --- Queries (main thread) ----------------------------------------------
    NavPathResult FindPath(const glm::vec3& start, const glm::vec3& goal,
                           const glm::vec3& searchExtents) const;
    // Nearest walkable point to `p` within `searchExtents`; false if none resident.
    bool SamplePoint(const glm::vec3& p, const glm::vec3& searchExtents, glm::vec3& out) const;
    // Is any resident poly within `searchExtents` of `p`? (agent missing-tile check)
    bool HasPolyNear(const glm::vec3& p, const glm::vec3& searchExtents) const;

    // Debug overlay: append resident navmesh triangles (world space, 3 verts each).
    void DebugTriangles(std::vector<glm::vec3>& outTris) const;

    const NavMeshInitParams& Params() const { return params_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NavMeshInitParams params_;
};

} // namespace nav
} // namespace hbe
