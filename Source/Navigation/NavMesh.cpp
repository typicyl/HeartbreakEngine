// Navigation/NavMesh.cpp - see NavMesh.h. All Detour lives here.
#include "Navigation/NavMesh.h"
#include "Navigation/NavTileCodec.h"

#include "Core/Log.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h" // dtNavMeshCreateParams (complete type for the mesh process)
#include "DetourNavMeshQuery.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "DetourCommon.h"
#include "DetourStatus.h"
#include "DetourAlloc.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace hbe {
namespace nav {
namespace {

// One area/flag pair. Everything walkable gets this; a query filter that includes it
// (the default 0xffff) traverses the whole navmesh. Distinct areas (water/road/door)
// and per-area cost are a later extension the format already leaves room for.
constexpr unsigned char kNavAreaGround = 1;
constexpr unsigned short kNavFlagWalk = 1;

constexpr int kMaxPathPolys = 512;    // corridor length cap for one query
constexpr int kMaxStraightPts = 512;  // path-corner cap for one query

// Pack a signed tile (x,y) into a stable 64-bit map key (bit-preserving cast).
inline u64 ColKey(i32 x, i32 y) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(y));
}

// Runtime poly-flag assignment for freshly built tiles (see NavMesh.h header note).
struct NavMeshProcess : public dtTileCacheMeshProcess {
    void process(dtNavMeshCreateParams* params, unsigned char* polyAreas,
                 unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA) polyAreas[i] = kNavAreaGround;
            polyFlags[i] = (polyAreas[i] != 0) ? kNavFlagWalk : 0;
        }
        // No off-mesh connections yet (the .hbnav format reserves space for them).
    }
};

inline int NextPow2(int v) {
    int r = 1;
    while (r < v) r <<= 1;
    return r;
}

} // namespace

struct NavMeshManager::Impl {
    dtNavMesh* navMesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtTileCache* tileCache = nullptr;
    NavTileCompressor comp;
    NavMeshProcess meshProc;
    std::unique_ptr<NavTileLinearAllocator> alloc;
    dtQueryFilter filter; // default: include 0xffff, exclude 0

    struct LayerRef {
        dtCompressedTileRef cref = 0;
        int tlayer = 0;
    };
    std::unordered_map<u64, std::vector<LayerRef>> columns;
    std::unordered_map<u32, dtObstacleRef> obstacles;
    u32 nextObstacleId = 1;
    bool obstaclesDirty = false;
    bool warnedTileBudget = false;

    ~Impl() {
        if (tileCache) dtFreeTileCache(tileCache);
        if (query) dtFreeNavMeshQuery(query);
        if (navMesh) dtFreeNavMesh(navMesh);
    }
};

NavMeshManager::NavMeshManager() = default;
NavMeshManager::~NavMeshManager() = default;

bool NavMeshManager::Ready() const { return impl_ && impl_->navMesh && impl_->query; }

bool NavMeshManager::Init(const NavMeshInitParams& p) {
    Shutdown();
    params_ = p;
    auto impl = std::make_unique<Impl>();

    // Tile/poly budget. The windowed navmesh only ever holds columns near the player,
    // so a modest budget keeps the polyRef salt bits healthy (>=10). Keep tileBits<=12
    // (<=4096 tiles) and polyBits=10 (1024 polys/tile) so salt stays >=10.
    constexpr int kLayersReserve = 8;
    int maxTiles = NextPow2(dtMax(1, p.maxResidentColumns) * kLayersReserve);
    maxTiles = dtClamp(maxTiles, 128, 4096);
    const int maxPolys = 1024;
    const int maxObstacles = dtClamp(p.maxObstacles, 16, 4096);

    // dtNavMesh.
    impl->navMesh = dtAllocNavMesh();
    if (!impl->navMesh) return false;
    dtNavMeshParams np{};
    np.orig[0] = p.origin.x;
    np.orig[1] = p.origin.y;
    np.orig[2] = p.origin.z;
    np.tileWidth = p.tileWorldSize;
    np.tileHeight = p.tileWorldSize;
    np.maxTiles = maxTiles;
    np.maxPolys = maxPolys;
    if (dtStatusFailed(impl->navMesh->init(&np))) {
        HBE_WARN("Nav: dtNavMesh init failed (maxTiles={}, maxPolys={}).", maxTiles, maxPolys);
        return false;
    }

    // dtNavMeshQuery (shared for all path queries).
    impl->query = dtAllocNavMeshQuery();
    if (!impl->query || dtStatusFailed(impl->query->init(impl->navMesh, 4096))) {
        HBE_WARN("Nav: dtNavMeshQuery init failed.");
        return false;
    }

    // dtTileCache (dynamic obstacles + per-tile rebuild). A 64KB linear arena covers a
    // single tile rebuild's scratch; it grows if a rebuild ever needs more.
    impl->alloc = std::make_unique<NavTileLinearAllocator>(64 * 1024);
    impl->tileCache = dtAllocTileCache();
    if (!impl->tileCache) return false;
    dtTileCacheParams tcp{};
    tcp.orig[0] = p.origin.x;
    tcp.orig[1] = p.origin.y;
    tcp.orig[2] = p.origin.z;
    tcp.cs = p.cellSize;
    tcp.ch = p.cellHeight;
    tcp.width = p.tileVoxels;
    tcp.height = p.tileVoxels;
    tcp.walkableHeight = p.agentHeight;
    tcp.walkableRadius = p.agentRadius;
    tcp.walkableClimb = p.agentMaxClimb;
    tcp.maxSimplificationError = 1.3f;
    tcp.maxTiles = maxTiles;
    tcp.maxObstacles = maxObstacles;
    if (dtStatusFailed(impl->tileCache->init(&tcp, impl->alloc.get(), &impl->comp, &impl->meshProc))) {
        HBE_WARN("Nav: dtTileCache init failed.");
        return false;
    }

    impl->filter.setIncludeFlags(0xffff);
    impl->filter.setExcludeFlags(0);
    impl_ = std::move(impl);
    return true;
}

void NavMeshManager::Shutdown() { impl_.reset(); }

bool NavMeshManager::AddColumn(i32 tx, i32 ty, const std::vector<u8>& blob) {
    if (!Ready()) return false;
    const u64 key = ColKey(tx, ty);
    if (impl_->columns.count(key)) return true; // already resident

    std::vector<Impl::LayerRef> layers;
    usize off = 0;
    while (off + 4 <= blob.size()) {
        u32 len = 0;
        std::memcpy(&len, blob.data() + off, 4);
        off += 4;
        if (len == 0 || off + len > blob.size()) break;
        // dtTileCache::addTile takes ownership of a dtAlloc'd buffer with FREE_DATA.
        unsigned char* data = static_cast<unsigned char*>(dtAlloc(len, DT_ALLOC_PERM));
        if (!data) break;
        std::memcpy(data, blob.data() + off, len);
        off += len;
        dtCompressedTileRef cref = 0;
        const dtStatus st = impl_->tileCache->addTile(data, static_cast<int>(len),
                                                      DT_COMPRESSEDTILE_FREE_DATA, &cref);
        if (dtStatusFailed(st)) {
            dtFree(data); // addTile did not take ownership on failure
            if (!impl_->warnedTileBudget) {
                HBE_WARN("Nav: tile cache full while streaming column ({},{}) - "
                         "streaming window may be too large for the tile budget.", tx, ty);
                impl_->warnedTileBudget = true;
            }
            continue;
        }
        const dtCompressedTile* ct = impl_->tileCache->getTileByRef(cref);
        const int tlayer = (ct && ct->header) ? ct->header->tlayer : 0;
        layers.push_back({cref, tlayer});
    }

    // Build the nav polys for this column. Safe even if some layers failed to add.
    impl_->tileCache->buildNavMeshTilesAt(tx, ty, impl_->navMesh);
    impl_->columns.emplace(key, std::move(layers));
    return true;
}

void NavMeshManager::RemoveColumn(i32 tx, i32 ty) {
    if (!Ready()) return;
    const u64 key = ColKey(tx, ty);
    auto it = impl_->columns.find(key);
    if (it == impl_->columns.end()) return;
    for (const Impl::LayerRef& lr : it->second) {
        // Remove the built nav tile first, then the compressed tile (which frees its data).
        const dtTileRef tref = impl_->navMesh->getTileRefAt(tx, ty, lr.tlayer);
        if (tref) impl_->navMesh->removeTile(tref, nullptr, nullptr);
        impl_->tileCache->removeTile(lr.cref, nullptr, nullptr);
    }
    impl_->columns.erase(it);
}

bool NavMeshManager::HasColumn(i32 tx, i32 ty) const {
    return Ready() && impl_->columns.count(ColKey(tx, ty)) != 0;
}

int NavMeshManager::ResidentColumnCount() const {
    return Ready() ? static_cast<int>(impl_->columns.size()) : 0;
}

bool NavMeshManager::ColumnBounds(i32 tx, i32 ty, glm::vec3& outMin, glm::vec3& outMax) const {
    if (!Ready()) return false;
    const f32 ts = params_.tileWorldSize;
    outMin = glm::vec3(params_.origin.x + tx * ts, -1e9f, params_.origin.z + ty * ts);
    outMax = glm::vec3(params_.origin.x + (tx + 1) * ts, 1e9f, params_.origin.z + (ty + 1) * ts);
    return true;
}

NavObstacleHandle NavMeshManager::AddObstacle(const glm::vec3& pos, f32 radius, f32 height) {
    NavObstacleHandle h;
    if (!Ready()) return h;
    const float p[3] = {pos.x, pos.y, pos.z};
    dtObstacleRef ref = 0;
    if (dtStatusFailed(impl_->tileCache->addObstacle(p, radius, height, &ref)) || ref == 0)
        return h; // cache full / too many pending; caller retries next frame
    h.id = impl_->nextObstacleId++;
    if (h.id == 0) h.id = impl_->nextObstacleId++; // never hand out 0
    impl_->obstacles[h.id] = ref;
    impl_->obstaclesDirty = true;
    return h;
}

void NavMeshManager::RemoveObstacle(NavObstacleHandle h) {
    if (!Ready() || !h.Valid()) return;
    auto it = impl_->obstacles.find(h.id);
    if (it == impl_->obstacles.end()) return;
    impl_->tileCache->removeObstacle(it->second);
    impl_->obstacles.erase(it);
    impl_->obstaclesDirty = true;
}

void NavMeshManager::UpdateObstacles(f32 dt) {
    if (!Ready()) return;
    bool upToDate = false;
    int guard = 0;
    // Drain the obstacle request/rebuild queue. Detour bounds each call to 64 requests
    // and 64 tile rebuilds, so a few passes settle a burst without unbounded cost.
    while (!upToDate && guard++ < 8)
        impl_->tileCache->update(dt, impl_->navMesh, &upToDate);
    if (upToDate) impl_->obstaclesDirty = false;
}

bool NavMeshManager::HasPendingObstacleWork() const {
    return Ready() && impl_->obstaclesDirty;
}

NavPathResult NavMeshManager::FindPath(const glm::vec3& start, const glm::vec3& goal,
                                       const glm::vec3& searchExtents) const {
    NavPathResult res;
    if (!Ready()) {
        res.missingTiles = true;
        return res;
    }
    const float ext[3] = {searchExtents.x, searchExtents.y, searchExtents.z};
    const float sp[3] = {start.x, start.y, start.z};
    const float gp[3] = {goal.x, goal.y, goal.z};

    dtPolyRef startRef = 0, endRef = 0;
    float startPt[3] = {sp[0], sp[1], sp[2]};
    float endPt[3] = {gp[0], gp[1], gp[2]};
    impl_->query->findNearestPoly(sp, ext, &impl_->filter, &startRef, startPt);
    impl_->query->findNearestPoly(gp, ext, &impl_->filter, &endRef, endPt);
    if (startRef == 0 || endRef == 0) {
        res.missingTiles = true; // start or goal has no resident poly -> stream + retry
        return res;
    }

    dtPolyRef path[kMaxPathPolys];
    int npath = 0;
    const dtStatus st = impl_->query->findPath(startRef, endRef, startPt, endPt,
                                               &impl_->filter, path, &npath, kMaxPathPolys);
    if (dtStatusFailed(st) || npath == 0) return res;
    res.partial = (st & DT_PARTIAL_RESULT) != 0;

    float straight[kMaxStraightPts * 3];
    unsigned char flags[kMaxStraightPts];
    dtPolyRef refs[kMaxStraightPts];
    int nstraight = 0;
    impl_->query->findStraightPath(startPt, endPt, path, npath, straight, flags, refs,
                                   &nstraight, kMaxStraightPts, 0);
    if (nstraight == 0) return res;
    res.corners.reserve(nstraight);
    for (int i = 0; i < nstraight; ++i)
        res.corners.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    res.found = true;
    return res;
}

bool NavMeshManager::SamplePoint(const glm::vec3& p, const glm::vec3& searchExtents,
                                 glm::vec3& out) const {
    if (!Ready()) return false;
    const float ext[3] = {searchExtents.x, searchExtents.y, searchExtents.z};
    const float c[3] = {p.x, p.y, p.z};
    dtPolyRef ref = 0;
    float nearest[3] = {p.x, p.y, p.z};
    impl_->query->findNearestPoly(c, ext, &impl_->filter, &ref, nearest);
    if (ref == 0) return false;
    out = glm::vec3(nearest[0], nearest[1], nearest[2]);
    return true;
}

bool NavMeshManager::HasPolyNear(const glm::vec3& p, const glm::vec3& searchExtents) const {
    if (!Ready()) return false;
    const float ext[3] = {searchExtents.x, searchExtents.y, searchExtents.z};
    const float c[3] = {p.x, p.y, p.z};
    dtPolyRef ref = 0;
    float nearest[3] = {0, 0, 0};
    impl_->query->findNearestPoly(c, ext, &impl_->filter, &ref, nearest);
    return ref != 0;
}

void NavMeshManager::DebugTriangles(std::vector<glm::vec3>& outTris) const {
    if (!Ready()) return;
    const dtNavMesh* nav = impl_->navMesh;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly* poly = &tile->polys[p];
            if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            const dtPolyDetail* pd = &tile->detailMeshes[p];
            for (int t = 0; t < pd->triCount; ++t) {
                const unsigned char* tri = &tile->detailTris[(pd->triBase + t) * 4];
                for (int k = 0; k < 3; ++k) {
                    const float* v;
                    if (tri[k] < poly->vertCount)
                        v = &tile->verts[poly->verts[tri[k]] * 3];
                    else
                        v = &tile->detailVerts[(pd->vertBase + (tri[k] - poly->vertCount)) * 3];
                    outTris.emplace_back(v[0], v[1], v[2]);
                }
            }
        }
    }
}

} // namespace nav
} // namespace hbe
