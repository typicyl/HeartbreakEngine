// Navigation/GridNav.h - real-time grid A* pathfinding with dynamic obstacles.
//
// The engine's only pathfinder (no external nav dependency): instead of baking,
// GridNav builds a lightweight CPU height index from the scene's STATIC geometry
// and runs A* on an implicit uniform grid, querying walkability on demand.
// Walls/cliffs are handled by a step-height rule; moving obstacles
// (NavigationObstacle) block cells analytically, so agents re-plan and route
// around them in real time - no bake, no asset files, no streaming. The index
// auto-rebuilds when the static set changes (level load / streamed cells).
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace hbe {

class Scene;

namespace nav {

struct GridNavParams {
    f32 cellSize = 0.5f;       // grid resolution (m); smaller = finer + slower
    f32 agentRadius = 0.4f;    // obstacle clearance
    f32 maxStep = 0.5f;        // climbable height between adjacent cells
    f32 maxSlopeDeg = 50.0f;   // steeper triangles aren't ground (= walls)
    f32 climb = 2.0f;          // how far above a reference height a cell's ground may be
    int maxExpand = 8000;      // A* node budget (caps cost of one query)
};

// A moving (or static) circular blocker for a path query.
struct GridObstacle {
    glm::vec3 pos{0.0f};
    f32 radius = 1.0f;
};

class GridNav : public NonCopyable {
public:
    void SetParams(const GridNavParams& p) { params_ = p; }
    const GridNavParams& Params() const { return params_; }

    // Rebuilds the height index from the scene's static geometry IF it changed
    // since the last call (cheap no-op otherwise). Call once per frame before
    // pathing. Returns Ready().
    bool EnsureBuilt(const Scene& scene, const std::filesystem::path& assetsDir);
    // Forces a rebuild from the current static geometry.
    void Rebuild(const Scene& scene, const std::filesystem::path& assetsDir);
    void Clear();
    bool Ready() const { return !tris_.empty(); }
    int TriangleCount() const { return static_cast<int>(tris_.size()); }

    // Highest walkable ground height at (x,z) at/under `nearY` (+climb); nullopt
    // if there is no walkable surface there.
    std::optional<f32> GroundAt(f32 x, f32 z, f32 nearY) const;

    // Grid A* from `start` to `goal` avoiding `obstacles`. Returns world-space
    // corners (snapped to the ground), empty if unreachable / not built.
    std::vector<glm::vec3> FindPath(const glm::vec3& start, const glm::vec3& goal,
                                    const std::vector<GridObstacle>& obstacles) const;

    // Debug: walkable cell centers within `extent` of `center` (viewport overlay).
    void DebugCells(const glm::vec3& center, f32 extent, std::vector<glm::vec3>& out) const;

private:
    struct Tri {
        glm::vec3 a, b, c;
        bool walkable = false; // slope within maxSlopeDeg
    };
    bool CellWalkable(int cx, int cz, f32 refY, const std::vector<GridObstacle>& obs,
                      f32& outGroundY) const;
    const std::vector<int>& BucketAt(f32 x, f32 z) const;

    GridNavParams params_;
    std::vector<Tri> tris_;            // world-space static triangles
    std::vector<std::vector<int>> buckets_; // XZ bucket -> triangle indices
    glm::vec2 gridMin_{0.0f};
    f32 bucket_ = 4.0f;
    int gw_ = 0, gh_ = 0;
    u64 signature_ = 0; // static-geometry fingerprint (rebuild when it changes)
};

// Steers every NavigationAgent toward its target along a GridNav A* path,
// re-planning periodically (and on target change) so it reroutes around moving
// NavigationObstacles. Snaps each agent onto the ground. Once per frame while
// the simulation runs.
void UpdateAgents(Scene& scene, const GridNav& grid, f32 dt);

} // namespace nav
} // namespace hbe
