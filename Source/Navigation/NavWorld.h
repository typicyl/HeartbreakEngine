// Navigation/NavWorld.h - the engine-owned navigation facade.
//
// NavWorld is the ONLY navigation type the rest of the engine touches. It exposes glm
// vectors and engine structs - never a dtNavMesh, dtPolyRef or dtTileCache - so
// gameplay/AI code is decoupled from Detour (which lives entirely inside NavMesh.cpp,
// reached through the pimpl here).
//
// It is a PERSISTENT, INDEPENDENTLY STREAMED manager: it loads a baked `.hbnav` once
// and streams individual tile columns in and out around the player on its OWN radius,
// decoupled from the level/geometry streaming. The world geometry can be unloaded while
// its navigation tiles stay resident (and vice-versa), so AI can path through regions
// whose visuals are not loaded. Tile reads run on the job system; Detour mutation and
// queries run on the main thread.
#pragma once

#include "Core/Types.h"
#include "Navigation/NavFormat.h" // NavStatus only (enum)

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hbe {
namespace nav {

// Streaming + query tuning. The nav radius is deliberately independent of - and by
// default WIDER than - geometry streaming, so AI can see navigation ahead of what is
// visually loaded. unloadRadius > loadRadius gives hysteresis (no thrash at the edge).
struct NavConfig {
    f32 loadRadius = 64.0f;   // world metres: stream a column in within this of a focus
    f32 unloadRadius = 96.0f; // ...and only unload it beyond this (hysteresis)
    int maxColumnLoadsPerFrame = 6;      // async reads kicked per frame
    int maxColumnFinalizePerFrame = 2;   // ready columns integrated into Detour per frame
    i32 maxResidentColumns = 256;        // window size (bounds the polyRef bit budget)
    glm::vec3 queryExtents{2.0f, 4.0f, 2.0f}; // snap box for start/goal poly lookup
};

class NavWorld : public NonCopyable {
public:
    NavWorld();
    ~NavWorld();

    // Load a baked .hbnav (relPath resolved against assetsDir). Replaces any current
    // navmesh (draining in-flight tile reads first). Returns true if the directory
    // parsed - tiles then stream in as the player moves. An empty relPath just unloads.
    bool Load(const std::filesystem::path& assetsDir, const std::string& relPath);
    void Unload();
    bool Loaded() const;
    NavStatus Status() const;
    const std::string& SourcePath() const;

    // Per-frame streaming (main thread, non-blocking). `foci` = player + camera, the
    // same set geometry streaming uses. Kicks async tile reads, integrates ready tiles
    // (budgeted), unloads departed ones, reconciles obstacles.
    void Update(const std::vector<glm::vec3>& foci, f32 dt);

    // Pathfinding. Returns true and fills `outCorners` (world-space path) on success.
    // When required tiles are not resident, `*outMissing` is set true and those tiles
    // are REQUESTED - the call never blocks; the caller keeps its current path and
    // retries in a later frame once streaming catches up.
    bool FindPath(const glm::vec3& start, const glm::vec3& goal,
                  std::vector<glm::vec3>& outCorners, bool* outMissing = nullptr);
    // Nearest walkable point to `p` (ground snap / target validation).
    bool SamplePoint(const glm::vec3& p, glm::vec3& out) const;
    // Is the navmesh loaded and the tile under `p` resident with walkable ground?
    bool IsResidentAt(const glm::vec3& p) const;
    // Ask the streamer to pull in the tiles around `p` (a far AI goal), so a later
    // FindPath can succeed. No-op if `p` is outside the baked grid.
    void RequestAround(const glm::vec3& p);

    // --- Dynamic obstacles --------------------------------------------------
    // One cylinder obstacle keyed by a stable caller id (e.g. an entity id).
    struct ObstacleDesc {
        u64 id = 0;
        glm::vec3 pos{0.0f};
        f32 radius = 1.0f;
        f32 height = 2.0f;
    };
    // Replace the whole obstacle set for this frame. NavWorld diffs against the last
    // call: it adds new ids, updates moved ones (remove+add - tile-cache obstacles are
    // static), and removes ids that vanished. Detour is touched only where something
    // actually changed, so calling this every frame is cheap when nothing moves.
    void SyncObstacles(const std::vector<ObstacleDesc>& obstacles);
    void ClearObstacles();

    // Diagnostics / editor overlay.
    int ResidentTileColumns() const;
    int TotalTileColumns() const;
    void DebugTriangles(std::vector<glm::vec3>& outTris) const;

    NavConfig& Config();
    const NavConfig& Config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nav
} // namespace hbe
