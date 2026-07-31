// Navigation/GridNav.h - real-time grid A* pathfinding with dynamic obstacles.
//
// The engine's only pathfinder (no external nav dependency): instead of baking,
// GridNav runs A* on an implicit uniform grid and queries walkability on demand
// from THREE surface providers, all behind GroundAt:
//
//   1. TERRAIN - ANALYTIC. A TerrainComponent is a height FUNCTION, so nav samples
//      `heights` directly (4 array reads for height AND slope) instead of storing
//      819k triangles for a 512 m terrain. Nothing is copied and nothing is
//      invalidated: the brush writes the same array nav reads, so a RUNTIME sculpt is
//      walkable the frame it lands and an EDITOR sculpt the frame after (the editor's
//      brush pass runs after nav in Engine::Run), with zero rebuild work either way.
//      Painted holes are not walkable and slopes past maxSlopeDeg are walls. The
//      surface sampled is terrain::SampleSurface - the SAME main-diagonal triangle the
//      renderer draws and Jolt collides.
//   2. STATIC MESHES - a world-space triangle soup in an XZ bucket index, rebuilt
//      when the authored static set changes (level load, or a mesh being moved or
//      repointed - the fingerprint hashes the world matrix and the mesh source).
//   3. STREAMED MESHES - the same soup, but inserted and removed INCREMENTALLY per
//      stream shard, so a shard spawning makes its props solid to AI without a
//      rebuild. (Before, StreamShard holders were excluded outright and a spawned
//      prop was invisible to AI.) A shard's geometry is treated as STATIC for as long
//      as it is resident; a streamed prop that MOVES belongs to AI as a
//      NavigationObstacle, not as re-indexed soup.
//
// Walls/cliffs are handled two ways: a step-height rule against walkable ground, AND a
// CLEARANCE test - a non-walkable triangle inside [ground, ground+agentHeight] blocks
// the cell, which is what makes a prop taller than `climb` solid to AI. Moving
// obstacles (NavigationObstacle) block cells analytically, so agents re-plan and route
// around them in real time - no bake, no asset files.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

class Scene;
struct TerrainComponent;

namespace nav {

struct GridNavParams {
    f32 cellSize = 0.5f;       // grid resolution (m); smaller = finer + slower
    f32 agentRadius = 0.4f;    // obstacle clearance
    f32 maxStep = 0.5f;        // climbable height between adjacent cells
    f32 maxSlopeDeg = 50.0f;   // steeper triangles aren't ground (= walls)
    f32 climb = 2.0f;          // how far above a reference height a cell's ground may be
    // Body height used for the CLEARANCE test. A cell whose ground is walkable is still
    // blocked when a non-walkable triangle (a wall, a crate side, a tree trunk) occupies
    // [ground, ground+agentHeight]. Without this, blocking came only from the step-height
    // rule against a prop's walkable TOP face, so anything taller than `climb` was
    // invisible: a 3 m container's walls were skipped as non-walkable and its roof was
    // out of climb range, and agents walked straight through it.
    f32 agentHeight = 1.8f;
    int maxExpand = 8000;      // A* node budget (caps cost of one query)
    // At most this many A* queries per frame across ALL agents (UpdateAgents). A single
    // long-range query on this grid is genuinely expensive - measured at 4.4 ms for a
    // 330 m path through dense props, against ~0.4 ms of frame headroom - so the budget
    // is 1: the cost becomes an occasional bounded spike instead of N stacked spikes,
    // and agents that miss out keep the path they have. (At 1/frame the pipeline still
    // supplies 120 queries/second, against ~3 Hz per agent of demand.) Reducing the
    // spike ITSELF needs hierarchical or time-sliced A*, which is a design change, not
    // a tuning knob.
    int maxQueriesPerFrame = 1;
};

// A moving (or static) circular blocker for a path query.
struct GridObstacle {
    glm::vec3 pos{0.0f};
    f32 radius = 1.0f;
};

// Which predicate selected the mesh geometry, derived from the ALWAYS-RESIDENT set.
// Reported so the editor's Navigation panel can state what the collector actually did
// instead of re-deriving it - the panel used to count every NavmeshInput entity
// (streamed ones included, which do not get a vote) and so could claim "3 tagged
// meshes" while the live filter was Static or All.
enum class NavSource { StaticLayer, NavmeshInputTag, AllMeshes };

class GridNav : public NonCopyable {
public:
    // A coarse XZ index over ONE query's obstacle list, built once in FindPath. The three
    // samplers used to linear-scan the whole list per sample, measured at 28.9 ns with 50
    // obstacles - four times the cost of the ground sample it accompanies, and the
    // dominant term of an entire query. Public only so --navbench can time it against the
    // linear scan it replaced.
    struct ObstacleGrid {
        void Build(const std::vector<GridObstacle>& obs, f32 pad);
        bool Blocked(f32 x, f32 z) const;

        std::vector<GridObstacle> padded; // radius already grown by the agent radius
        std::vector<std::vector<int>> cells;
        glm::vec2 mn{0.0f};
        f32 cell = 4.0f;
        int w = 0, h = 0;
        bool empty = true;
    };

    // Params feed the slope test and the bucket size, so changing them invalidates
    // the index - they are part of the fingerprint EnsureBuilt watches.
    void SetParams(const GridNavParams& p);
    const GridNavParams& Params() const { return params_; }

    // Refreshes the terrain surfaces + streamed geometry and rebuilds the static
    // height index IF the static set changed since the last call (cheap no-op
    // otherwise). Call once per frame before pathing. Returns Ready().
    bool EnsureBuilt(const Scene& scene, const std::filesystem::path& assetsDir);
    // Forces a full rebuild from the current static geometry (streamed blocks are
    // re-indexed from memory, not re-read from disk).
    void Rebuild(const Scene& scene, const std::filesystem::path& assetsDir);
    // Drops the index. Keeps the mesh CPU-geometry cache (see DropGeometryCache) -
    // Clear is a per-level teardown, not an asset-reimport.
    void Clear();
    // Forgets decoded mesh geometry, so a REIMPORTED asset is re-read. The cache is
    // deliberately not tied to Clear/Rebuild: a full rebuild used to re-read and
    // re-decode every nav mesh from disk on the main thread, which is what made an
    // enemy spawn mid-combat cost multiple milliseconds.
    void DropGeometryCache() { meshCache_.clear(); }
    bool Ready() const { return liveTris_ > 0 || !terrains_.empty(); }
    int TriangleCount() const { return liveTris_; }
    // Diagnostics (self-tests + the editor's Navigation panel).
    int TerrainCount() const { return static_cast<int>(terrains_.size()); }
    int StaticTriangleCount() const { return staticTris_; }
    int StreamedBlockCount() const { return static_cast<int>(dyn_.size()); }
    bool HasStreamedShard(u32 shardIndex) const;
    // What the last EnsureBuilt/Rebuild actually selected (see NavSource).
    NavSource ActiveSource() const { return source_; }
    int AcceptedMeshCount() const { return acceptedMeshes_; }
    // Full rebuilds so far. A terrain sculpt, and a shard spawn/despawn, must NOT
    // increment this - that is the whole point of providers 1 and 3.
    u32 RebuildCount() const { return rebuilds_; }
    // A* queries run by the last UpdateAgents call (perf band diagnostics).
    u32 LastQueryCount() const { return lastQueries_; }
    void ResetQueryCount() const { lastQueries_ = 0; }
    void NoteQuery() const { ++lastQueries_; }

    // --- Benchmark hooks (--navbench only) -----------------------------------
    // Let the harness time the CHEAP GATE and the EXPENSIVE per-shard fingerprint
    // separately, so the streamed-poll before/after is one binary measuring both paths
    // rather than a claim about a build that no longer exists. Not used by the engine.
    void BenchStreamPoll(const Scene& scene, const std::filesystem::path& assetsDir,
                         bool forceFullFingerprint);
    // Ground sample with the clearance test, timed without A* around it.
    std::optional<f32> BenchGround(f32 x, f32 z, f32 nearY, bool clearance) const {
        return Ground(x, z, nearY, clearance);
    }
    // Linear obstacle scan (what the three samplers did before the ObstacleGrid), so the
    // grid's win is measured against the real alternative in the same process.
    static bool BenchLinearBlocked(const std::vector<GridObstacle>& obs, f32 pad, f32 x, f32 z);

    // Walkable ground height at (x,z) whose height is closest to `nearY` (within
    // `climb`); nullopt if there is no walkable surface there. Does NOT apply the
    // clearance test - this is the "ride the ground" query, not the "can I stand
    // here" query (see CellWalkable).
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
        bool live = false;     // false = a freed slot (removed streamed block)
    };

    // An analytic heightfield surface, BORROWED for the CURRENT FRAME ONLY.
    // `comp` points straight at the live TerrainComponent so a brush stroke needs
    // no invalidation - which also means the pointer must never outlive the frame
    // (terrain::EnsureHeights can reallocate `heights` on a resolution change).
    struct TerrainSurface {
        const TerrainComponent* comp = nullptr;
        entt::entity entity = entt::null; // stable identity for the fingerprint
        glm::mat4 world{1.0f};
        glm::mat4 invWorld{1.0f};
        f32 halfExtent = 0.0f;  // local XZ half side
        f32 gradScaleX = 1.0f;  // local dh/dx -> world gradient (yScale / xScale)
        f32 gradScaleZ = 1.0f;
    };

    // Positions-only CPU geometry, cached per mesh source so N submesh entities on
    // one `.uaf` cost ONE read (the level-load hot spot: 6 submeshes of a 3.3 MB
    // file used to be 6 full reads + 6 full decodes) and so a streamed block can be
    // indexed with no disk touch at all.
    struct NavGeom {
        std::vector<glm::vec3> pos;
        std::vector<i32> idx;
    };

    // A block of geometry indexed INCREMENTALLY (one stream shard). Owned, so a
    // full static rebuild can re-insert it without re-reading anything.
    struct DynBlock {
        u32 shard = 0;
        u64 sig = 0;             // content fingerprint of the shard's mesh entities
        std::vector<Tri> tris;   // world-space, walkability already resolved
        std::vector<int> slots;  // where they live in tris_
    };

    // Ground resolution shared by every walkability test. `requireClearance` adds the
    // body-volume test against non-walkable triangles (see GridNavParams::agentHeight).
    std::optional<f32> Ground(f32 x, f32 z, f32 nearY, bool requireClearance) const;
    // `skipClearance` exempts the cells immediately around the agent's own position. An
    // agent standing INSIDE geometry (a streamed shard can now spawn a prop on top of
    // one, and nav agents drive their Transform directly rather than through the
    // character controller) would otherwise have all eight neighbours rejected by the
    // clearance test and never path anywhere again. Exempting one ring lets it walk out
    // over two frames without weakening the test anywhere it matters.
    bool CellWalkable(int cx, int cz, f32 refY, const ObstacleGrid& obs, f32& outGroundY,
                      bool skipClearance = false) const;
    const std::vector<int>& BucketAt(f32 x, f32 z) const;

    // One pass over the accepted static set producing both fingerprint tiers (the heavy
    // terms only when asked). See cheapSig_/heavySig_.
    void ScanStatic(const Scene& scene, bool withHeavy, u64& outCheap, u64& outHeavy,
                    u32& outCount);
    void RefreshTerrains(const Scene& scene);
    void SyncStreamedGeometry(const Scene& scene, const std::filesystem::path& assetsDir);
    const NavGeom* ResolveGeom(const std::filesystem::path& assetsDir, const std::string& source);
    void AppendTris(const NavGeom& g, const glm::mat4& world, std::vector<Tri>& out) const;
    void BuildBuckets();                  // (re)sizes the XZ index and inserts every live tri
    bool IndexTri(int slot);              // false = outside the current grid
    void UnindexTri(int slot);
    void AddBlock(DynBlock&& block);
    void RemoveBlock(DynBlock& block);

    GridNavParams params_;
    f32 cosSlope_ = 0.642787f;              // cos(maxSlopeDeg), kept with params_
    std::vector<Tri> tris_;                 // world-space triangles (static + streamed)
    std::vector<int> freeTris_;             // reusable slots in tris_
    int liveTris_ = 0;
    int staticTris_ = 0;                    // slots owned by the authored static set
    std::vector<std::vector<int>> buckets_; // XZ bucket -> triangle indices
    std::vector<TerrainSurface> terrains_;
    std::vector<DynBlock> dyn_;
    std::unordered_map<std::string, std::shared_ptr<NavGeom>> meshCache_;
    glm::vec2 gridMin_{0.0f};
    f32 bucket_ = 4.0f;
    int gw_ = 0, gh_ = 0;
    // --- Static fingerprint, in two tiers -------------------------------------
    // `cheapSig_` (entity ids + count + terrain layout + params) runs EVERY frame and
    // catches anything that adds or removes nav geometry - a level load, a spawn, a
    // terrain being added or resized. Measured at 0.027 ms over 4000 mesh entities.
    //
    // `heavySig_` additionally folds in each accepted entity's WORLD MATRIX and MESH
    // SOURCE, which is what makes "somebody dragged a static wall with the gizmo" and
    // "somebody repointed a MeshRef" visible at all - without them the fingerprint
    // matched, no rebuild fired, and agents pathed through the wall's old position
    // forever. That costs 0.216 ms over the same set (a WorldMatrix walks the parent
    // chain and builds a mat4; the source hash walks a path string), which is more than
    // half the frame's entire CPU headroom to spend every frame on a question whose
    // answer changes only when a human drags something.
    //
    // So it runs every kHeavyFingerprintPeriod frames instead - amortized to ~0.027 ms -
    // and the cost of that choice is bounded and stated: an edit to a static mesh's
    // PLACEMENT is picked up within 8 frames (~70 ms at 120 Hz) rather than the next
    // frame. Anything that changes the entity SET is still same-frame.
    static constexpr u64 kHeavyFingerprintPeriod = 8;
    u64 cheapSig_ = 0;
    u64 heavySig_ = 0;
    u64 fpFrame_ = 0;
    bool heavyValid_ = false;
    // Fingerprint of the RESIDENT STREAMED ENTITY SET. Cheap on purpose: entity id +
    // shard index only, no string hash and no transform. Re-deriving a per-entity
    // content hash every frame cost 0.114 ms at 2000 streamed entities - 28% of the
    // whole frame's CPU headroom - to detect something that changes only when the
    // streamer spawns or despawns a shard. A shard's CONTENT is fixed by the bake, so
    // the entity set is a complete change signal. (Consequence, deliberate: a streamed
    // prop that MOVES is not re-indexed. Moving geometry belongs to AI as a
    // NavigationObstacle, not as static soup - re-indexing it per frame was a full
    // unindex+reindex of the whole shard every frame.)
    u64 streamSig_ = 0;
    bool streamSigValid_ = false;
    bool streamPending_ = false; // shards the per-frame budget has not reached yet
    NavSource source_ = NavSource::AllMeshes;
    int acceptedMeshes_ = 0;
    u32 rebuilds_ = 0;
    mutable u32 lastQueries_ = 0;
    bool warnedTilt_ = false;
};

// Steers every NavigationAgent toward its target along a GridNav A* path,
// re-planning periodically (and on target change) so it reroutes around moving
// NavigationObstacles. Snaps each agent onto the ground. Once per frame while
// the simulation runs.
void UpdateAgents(Scene& scene, const GridNav& grid, f32 dt);

} // namespace nav
} // namespace hbe
