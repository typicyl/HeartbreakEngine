// Navigation/NavWorld.cpp - see NavWorld.h.
#include "Navigation/NavWorld.h"

#include "Navigation/NavFormat.h"
#include "Navigation/NavMesh.h"

#include "Core/JobSystem.h"
#include "Core/Log.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hbe {
namespace nav {
namespace {

inline u64 ColKey(i32 x, i32 y) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(y));
}

} // namespace

struct NavWorld::Impl {
    NavConfig config;
    NavMeshManager mgr;
    NavMeshData data;
    std::filesystem::path assetsDir;
    std::string relPath;
    std::unordered_map<u64, int> indexByCoord; // ColKey -> record index in data.tiles

    // Per-column streaming record. Held by unique_ptr so the atomic has a stable
    // address for the worker job (the same discipline the tag streamer uses).
    struct ColStream {
        i32 tx = 0, ty = 0;
        int rec = -1;
        std::atomic<int> state{0}; // 0 idle, 1 loading, 2 ready, 3 resident, 4 failed
        std::vector<u8> blob;
        Impl* owner = nullptr;
    };
    std::unordered_map<u64, std::unique_ptr<ColStream>> streams;
    std::atomic<int> inFlight{0};

    struct Obs {
        NavObstacleHandle handle;
        glm::vec3 pos{0.0f};
        f32 radius = 0.0f;
        f32 height = 0.0f;
        bool enabled = false;
    };
    std::unordered_map<u64, Obs> obstacles;

    i32 TileX(f32 wx) const {
        return static_cast<i32>(std::floor((wx - data.origin[0]) / data.tileWorldSize));
    }
    i32 TileZ(f32 wz) const {
        return static_cast<i32>(std::floor((wz - data.origin[2]) / data.tileWorldSize));
    }

    // Distance from a world point to a tile column's XZ rectangle (0 if inside).
    f32 BoxDistXZ(const glm::vec3& p, i32 tx, i32 ty) const {
        const f32 ts = data.tileWorldSize;
        const f32 minx = data.origin[0] + tx * ts;
        const f32 minz = data.origin[2] + ty * ts;
        const f32 maxx = minx + ts;
        const f32 maxz = minz + ts;
        const f32 dx = std::max({minx - p.x, 0.0f, p.x - maxx});
        const f32 dz = std::max({minz - p.z, 0.0f, p.z - maxz});
        return std::sqrt(dx * dx + dz * dz);
    }

    void DrainInFlight() {
        while (inFlight.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
    }

    static void LoadJob(void* arg) {
        auto* cs = static_cast<ColStream*>(arg);
        Impl* self = cs->owner;
        bool ok = false;
        if (cs->rec >= 0 && cs->rec < static_cast<int>(self->data.tiles.size()))
            ok = ReadTileBlob(self->data, self->data.tiles[cs->rec], cs->blob);
        cs->state.store(ok ? 2 : 4, std::memory_order_release);
        self->inFlight.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Create + kick a load for a column if it is baked and not already streaming/resident.
    void KickLoad(i32 tx, i32 ty) {
        const u64 key = ColKey(tx, ty);
        auto ix = indexByCoord.find(key);
        if (ix == indexByCoord.end()) return; // no baked tile here
        if (mgr.HasColumn(tx, ty)) return;
        if (streams.count(key)) return; // already loading/ready
        auto cs = std::make_unique<ColStream>();
        cs->tx = tx;
        cs->ty = ty;
        cs->rec = ix->second;
        cs->owner = this;
        cs->state.store(1, std::memory_order_release);
        ColStream* raw = cs.get();
        streams.emplace(key, std::move(cs));
        inFlight.fetch_add(1, std::memory_order_acquire);
        if (jobs::IsInitialized())
            jobs::RunDetached(&Impl::LoadJob, raw, jobs::Priority::Normal);
        else
            LoadJob(raw); // headless / no job system: stage inline
    }

    // Re-apply obstacles overlapping a just-resident column so the freshly built tile
    // accounts for them (a tile added AFTER an obstacle is not in the obstacle's touched
    // set, so buildNavMeshTilesAt would ignore it otherwise).
    void ReapplyObstaclesOverlapping(i32 tx, i32 ty) {
        glm::vec3 cmin, cmax;
        if (!mgr.ColumnBounds(tx, ty, cmin, cmax)) return;
        for (auto& [id, ob] : obstacles) {
            if (!ob.enabled || !ob.handle.Valid()) continue;
            if (ob.pos.x + ob.radius < cmin.x || ob.pos.x - ob.radius > cmax.x) continue;
            if (ob.pos.z + ob.radius < cmin.z || ob.pos.z - ob.radius > cmax.z) continue;
            mgr.RemoveObstacle(ob.handle);
            ob.handle = mgr.AddObstacle(ob.pos, ob.radius, ob.height);
        }
    }
};

NavWorld::NavWorld() : impl_(std::make_unique<Impl>()) {}
NavWorld::~NavWorld() { Unload(); }

bool NavWorld::Load(const std::filesystem::path& assetsDir, const std::string& relPath) {
    Unload();
    impl_->assetsDir = assetsDir;
    impl_->relPath = relPath;
    if (relPath.empty()) return true;

    impl_->data = LoadNavMesh(assetsDir / relPath);
    if (!impl_->data.Valid()) {
        HBE_WARN("Nav: navmesh '{}' not loaded (status {}).", relPath,
                 static_cast<u32>(impl_->data.status));
        return false;
    }
    // Coord -> record index.
    impl_->indexByCoord.reserve(impl_->data.tiles.size());
    for (int i = 0; i < static_cast<int>(impl_->data.tiles.size()); ++i)
        impl_->indexByCoord[ColKey(impl_->data.tiles[i].x, impl_->data.tiles[i].y)] = i;

    // Use the first agent profile's radius/height/climb (single-profile runtime for now).
    NavMeshInitParams p;
    p.cellSize = impl_->data.cellSize;
    p.cellHeight = impl_->data.cellHeight;
    p.tileVoxels = impl_->data.tileVoxels;
    p.tileWorldSize = impl_->data.tileWorldSize;
    p.origin = glm::vec3(impl_->data.origin[0], impl_->data.origin[1], impl_->data.origin[2]);
    if (!impl_->data.profiles.empty()) {
        const NavAgentProfile& pr = impl_->data.profiles.front();
        p.agentRadius = pr.radius;
        p.agentHeight = pr.height;
        p.agentMaxClimb = pr.maxClimb;
    }
    p.maxResidentColumns = impl_->config.maxResidentColumns;
    if (!impl_->mgr.Init(p)) {
        HBE_WARN("Nav: manager init failed for '{}'.", relPath);
        impl_->data = NavMeshData{};
        return false;
    }
    HBE_INFO("Nav: loaded '{}' ({} tile columns, {} profiles).", relPath,
             impl_->data.tiles.size(), impl_->data.profiles.size());
    return true;
}

void NavWorld::Unload() {
    impl_->DrainInFlight();
    impl_->mgr.Shutdown();
    impl_->streams.clear();
    impl_->indexByCoord.clear();
    impl_->obstacles.clear();
    impl_->data = NavMeshData{};
    impl_->relPath.clear();
}

bool NavWorld::Loaded() const { return impl_->data.Valid() && impl_->mgr.Ready(); }
NavStatus NavWorld::Status() const { return impl_->data.status; }
const std::string& NavWorld::SourcePath() const { return impl_->relPath; }
NavConfig& NavWorld::Config() { return impl_->config; }
const NavConfig& NavWorld::Config() const { return impl_->config; }

void NavWorld::Update(const std::vector<glm::vec3>& foci, f32 dt) {
    if (!Loaded()) return;
    const NavConfig& cfg = impl_->config;

    // 1. Finalize ready columns (budgeted). Track newly-resident coords for obstacle
    //    reconciliation.
    int finalized = 0;
    std::vector<std::pair<i32, i32>> newlyResident;
    for (auto& [key, cs] : impl_->streams) {
        if (finalized >= cfg.maxColumnFinalizePerFrame) break;
        const int st = cs->state.load(std::memory_order_acquire);
        if (st != 2 && st != 4) continue;
        if (st == 4) { // failed read
            cs->state.store(3, std::memory_order_release); // mark handled; skip re-kick
            continue;
        }
        impl_->mgr.AddColumn(cs->tx, cs->ty, cs->blob);
        cs->blob.clear();
        cs->blob.shrink_to_fit();
        cs->state.store(3, std::memory_order_release);
        newlyResident.emplace_back(cs->tx, cs->ty);
        ++finalized;
    }

    // 2. Unload resident columns that drifted beyond unloadRadius of every focus.
    std::vector<u64> toErase;
    for (auto& [key, cs] : impl_->streams) {
        if (cs->state.load(std::memory_order_acquire) != 3) continue;
        if (!impl_->mgr.HasColumn(cs->tx, cs->ty)) { toErase.push_back(key); continue; }
        f32 nearest = 1e30f;
        for (const glm::vec3& f : foci)
            nearest = std::min(nearest, impl_->BoxDistXZ(f, cs->tx, cs->ty));
        if (nearest > cfg.unloadRadius) {
            impl_->mgr.RemoveColumn(cs->tx, cs->ty);
            toErase.push_back(key);
        }
    }
    for (u64 k : toErase) impl_->streams.erase(k);

    // 3. Kick loads for desired-but-absent columns (budgeted). Scan only the tile range
    //    around each focus, so this is O(window), not O(world).
    int kicked = 0;
    if (impl_->data.tileWorldSize > 0.0f) {
        const int rt = static_cast<int>(std::ceil(cfg.loadRadius / impl_->data.tileWorldSize)) + 1;
        for (const glm::vec3& f : foci) {
            if (kicked >= cfg.maxColumnLoadsPerFrame) break;
            const i32 ftx = impl_->TileX(f.x), fty = impl_->TileZ(f.z);
            for (i32 ty = fty - rt; ty <= fty + rt && kicked < cfg.maxColumnLoadsPerFrame; ++ty)
                for (i32 tx = ftx - rt; tx <= ftx + rt && kicked < cfg.maxColumnLoadsPerFrame; ++tx) {
                    const u64 key = ColKey(tx, ty);
                    if (!impl_->indexByCoord.count(key)) continue;
                    if (impl_->mgr.HasColumn(tx, ty) || impl_->streams.count(key)) continue;
                    if (impl_->BoxDistXZ(f, tx, ty) > cfg.loadRadius) continue;
                    impl_->KickLoad(tx, ty);
                    ++kicked;
                }
        }
    }

    // 4. Obstacle reconciliation + tile-cache rebuild.
    for (const auto& [tx, ty] : newlyResident) impl_->ReapplyObstaclesOverlapping(tx, ty);
    impl_->mgr.UpdateObstacles(dt);
}

bool NavWorld::FindPath(const glm::vec3& start, const glm::vec3& goal,
                        std::vector<glm::vec3>& outCorners, bool* outMissing) {
    outCorners.clear();
    if (outMissing) *outMissing = false;
    if (!Loaded()) {
        if (outMissing) *outMissing = true;
        return false;
    }
    NavPathResult r = impl_->mgr.FindPath(start, goal, impl_->config.queryExtents);
    if (r.missingTiles) {
        if (outMissing) *outMissing = true;
        RequestAround(start);
        RequestAround(goal);
        return false;
    }
    if (!r.found) return false;
    outCorners = std::move(r.corners);
    return true;
}

bool NavWorld::SamplePoint(const glm::vec3& p, glm::vec3& out) const {
    if (!Loaded()) return false;
    return impl_->mgr.SamplePoint(p, impl_->config.queryExtents, out);
}

bool NavWorld::IsResidentAt(const glm::vec3& p) const {
    if (!Loaded()) return false;
    if (!impl_->mgr.HasColumn(impl_->TileX(p.x), impl_->TileZ(p.z))) return false;
    return impl_->mgr.HasPolyNear(p, impl_->config.queryExtents);
}

void NavWorld::RequestAround(const glm::vec3& p) {
    if (!Loaded() || impl_->data.tileWorldSize <= 0.0f) return;
    const i32 ptx = impl_->TileX(p.x), pty = impl_->TileZ(p.z);
    for (i32 ty = pty - 1; ty <= pty + 1; ++ty)
        for (i32 tx = ptx - 1; tx <= ptx + 1; ++tx)
            impl_->KickLoad(tx, ty);
}

void NavWorld::SyncObstacles(const std::vector<ObstacleDesc>& list) {
    if (!Loaded()) return;
    std::unordered_set<u64> seen;
    seen.reserve(list.size());
    for (const ObstacleDesc& d : list) {
        seen.insert(d.id);
        Impl::Obs& ob = impl_->obstacles[d.id];
        const bool existed = ob.handle.Valid();
        // dtTileCache obstacles are static, so a move is remove+add; skip when nothing
        // crossed the epsilon so a still crate costs nothing.
        const bool changed = !existed || glm::distance(ob.pos, d.pos) > 0.05f ||
                             std::abs(ob.radius - d.radius) > 0.05f ||
                             std::abs(ob.height - d.height) > 0.05f;
        if (!changed) continue;
        if (existed) impl_->mgr.RemoveObstacle(ob.handle);
        ob.handle = impl_->mgr.AddObstacle(d.pos, d.radius, d.height);
        ob.pos = d.pos;
        ob.radius = d.radius;
        ob.height = d.height;
        ob.enabled = true;
    }
    // Drop obstacles whose id disappeared this frame (entity destroyed / disabled).
    for (auto it = impl_->obstacles.begin(); it != impl_->obstacles.end();) {
        if (!seen.count(it->first)) {
            if (it->second.handle.Valid()) impl_->mgr.RemoveObstacle(it->second.handle);
            it = impl_->obstacles.erase(it);
        } else {
            ++it;
        }
    }
}

void NavWorld::ClearObstacles() {
    if (!impl_->mgr.Ready()) { impl_->obstacles.clear(); return; }
    for (auto& [id, ob] : impl_->obstacles)
        if (ob.handle.Valid()) impl_->mgr.RemoveObstacle(ob.handle);
    impl_->obstacles.clear();
}

int NavWorld::ResidentTileColumns() const { return impl_->mgr.ResidentColumnCount(); }
int NavWorld::TotalTileColumns() const { return static_cast<int>(impl_->data.tiles.size()); }
void NavWorld::DebugTriangles(std::vector<glm::vec3>& outTris) const {
    impl_->mgr.DebugTriangles(outTris);
}

} // namespace nav
} // namespace hbe
