// Navigation/NavBaker.cpp - see NavBaker.h. Recast lives ONLY here (editor build).
#include "Navigation/NavBaker.h"

#include "Navigation/NavFormat.h"
#include "Navigation/NavMesh.h"
#include "Navigation/NavTileCodec.h"

#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Scene/BrushSystem.h" // brush::BuildEntityMesh (CSG blockout brushes as nav geometry)
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/StrokeZone.h" // strokezone::IsStroke - decals are never nav geometry

#include "Recast.h"
#include "DetourTileCacheBuilder.h"
#include "DetourStatus.h"
#include "DetourAlloc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace hbe {
namespace nav {
namespace {

// FNV-1a over the world-space geometry + the build settings, so the editor can detect a
// scene changed since the last bake.
u64 HashGeometry(const std::vector<f32>& verts, const std::vector<i32>& tris,
                 const NavBuildSettings& s) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const u64 nv = verts.size(), nt = tris.size();
    mix(&nv, sizeof(nv));
    mix(&nt, sizeof(nt));
    if (!verts.empty()) mix(verts.data(), verts.size() * sizeof(f32));
    if (!tris.empty()) mix(tris.data(), tris.size() * sizeof(i32));
    mix(&s.cellSize, sizeof(f32));
    mix(&s.cellHeight, sizeof(f32));
    mix(&s.tileVoxels, sizeof(i32));
    mix(&s.agentRadius, sizeof(f32));
    mix(&s.agentHeight, sizeof(f32));
    mix(&s.agentMaxClimb, sizeof(f32));
    mix(&s.agentMaxSlopeDeg, sizeof(f32));
    return h;
}

// RAII for Recast's per-tile scratch (freed on any early return).
struct RcScratch {
    rcHeightfield* solid = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcHeightfieldLayerSet* lset = nullptr;
    ~RcScratch() {
        if (solid) rcFreeHeightField(solid);
        if (chf) rcFreeCompactHeightfield(chf);
        if (lset) rcFreeHeightfieldLayerSet(lset);
    }
};

// Voxelise the triangles overlapping one tile column into compressed DetourTileCache
// layers. `triList` are triangle indices into `tris`. Mirrors RecastDemo's
// rasterizeTileLayers. Returns false on a hard failure (empty tile is "true, 0 layers").
bool RasterizeColumn(rcContext& ctx, const std::vector<f32>& verts, int nverts,
                     const std::vector<i32>& tris, const std::vector<int>& triList,
                     const rcConfig& cfg, int tx, int ty, NavTileCompressor& comp,
                     NavTileBuild& out) {
    if (triList.empty()) return true;
    const float tcs = cfg.tileSize * cfg.cs;
    rcConfig tcfg = cfg;
    tcfg.bmin[0] = cfg.bmin[0] + tx * tcs;
    tcfg.bmin[1] = cfg.bmin[1];
    tcfg.bmin[2] = cfg.bmin[2] + ty * tcs;
    tcfg.bmax[0] = cfg.bmin[0] + (tx + 1) * tcs;
    tcfg.bmax[1] = cfg.bmax[1];
    tcfg.bmax[2] = cfg.bmin[2] + (ty + 1) * tcs;
    tcfg.bmin[0] -= tcfg.borderSize * tcfg.cs;
    tcfg.bmin[2] -= tcfg.borderSize * tcfg.cs;
    tcfg.bmax[0] += tcfg.borderSize * tcfg.cs;
    tcfg.bmax[2] += tcfg.borderSize * tcfg.cs;

    RcScratch rc;
    rc.solid = rcAllocHeightfield();
    if (!rc.solid || !rcCreateHeightfield(&ctx, *rc.solid, tcfg.width, tcfg.height, tcfg.bmin,
                                          tcfg.bmax, tcfg.cs, tcfg.ch))
        return false;

    // Expand the triangle-index list into a vertex-index list Recast can rasterize.
    std::vector<int> ltris;
    ltris.reserve(triList.size() * 3);
    for (int t : triList) {
        ltris.push_back(tris[t * 3 + 0]);
        ltris.push_back(tris[t * 3 + 1]);
        ltris.push_back(tris[t * 3 + 2]);
    }
    const int nlt = static_cast<int>(triList.size());
    std::vector<unsigned char> areas(static_cast<size_t>(nlt), 0);
    rcMarkWalkableTriangles(&ctx, tcfg.walkableSlopeAngle, verts.data(), nverts, ltris.data(),
                            nlt, areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, ltris.data(), areas.data(), nlt,
                              *rc.solid, tcfg.walkableClimb))
        return false;

    rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb, *rc.solid);
    rcFilterLedgeSpans(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *rc.solid);
    rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight, *rc.solid);

    rc.chf = rcAllocCompactHeightfield();
    if (!rc.chf || !rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight, tcfg.walkableClimb,
                                              *rc.solid, *rc.chf))
        return false;
    if (!rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *rc.chf)) return false;

    rc.lset = rcAllocHeightfieldLayerSet();
    if (!rc.lset ||
        !rcBuildHeightfieldLayers(&ctx, *rc.chf, tcfg.borderSize, tcfg.walkableHeight, *rc.lset))
        return false;

    for (int i = 0; i < rc.lset->nlayers; ++i) {
        const rcHeightfieldLayer* layer = &rc.lset->layers[i];
        dtTileCacheLayerHeader header{};
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = tx;
        header.ty = ty;
        header.tlayer = i;
        rcVcopy(header.bmin, layer->bmin);
        rcVcopy(header.bmax, layer->bmax);
        header.width = static_cast<unsigned char>(layer->width);
        header.height = static_cast<unsigned char>(layer->height);
        header.minx = static_cast<unsigned char>(layer->minx);
        header.maxx = static_cast<unsigned char>(layer->maxx);
        header.miny = static_cast<unsigned char>(layer->miny);
        header.maxy = static_cast<unsigned char>(layer->maxy);
        header.hmin = static_cast<unsigned short>(layer->hmin);
        header.hmax = static_cast<unsigned short>(layer->hmax);

        unsigned char* data = nullptr;
        int dataSize = 0;
        const dtStatus st = dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas,
                                                  layer->cons, &data, &dataSize);
        if (dtStatusFailed(st) || !data || dataSize <= 0) {
            if (data) dtFree(data);
            continue;
        }
        out.layers.emplace_back(data, data + dataSize);
        dtFree(data);
    }

    // Tight column bounds (the tile rect, not the border-expanded rasterization box).
    out.bmin[0] = cfg.bmin[0] + tx * tcs;
    out.bmin[1] = cfg.bmin[1];
    out.bmin[2] = cfg.bmin[2] + ty * tcs;
    out.bmax[0] = cfg.bmin[0] + (tx + 1) * tcs;
    out.bmax[1] = cfg.bmax[1];
    out.bmax[2] = cfg.bmin[2] + (ty + 1) * tcs;
    return true;
}

// Gather world-space triangles from the scene's navigation-input meshes.
void GatherGeometry(const Scene& scene, const std::filesystem::path& assetsDir,
                    std::vector<f32>& verts, std::vector<i32>& tris) {
    const entt::registry& reg = scene.Registry();
    std::unordered_map<std::string, std::shared_ptr<std::pair<std::vector<glm::vec3>, std::vector<i32>>>> cache;

    const auto resolve = [&](const std::string& source)
        -> std::shared_ptr<std::pair<std::vector<glm::vec3>, std::vector<i32>>> {
        if (source.empty()) return nullptr;
        auto it = cache.find(source);
        if (it != cache.end()) return it->second;
        auto geom = std::make_shared<std::pair<std::vector<glm::vec3>, std::vector<i32>>>();
        const auto adopt = [&](const MeshData& md) {
            geom->first.reserve(md.vertices.size());
            for (const Vertex& v : md.vertices) geom->first.push_back(v.position);
            geom->second.reserve(md.indices.size());
            for (const u32 i : md.indices) geom->second.push_back(static_cast<i32>(i));
        };
        if (source.rfind("prim:", 0) == 0) {
            MeshData md = mesh::GeneratePrimitive(source.substr(5));
            if (!md.vertices.empty()) adopt(md);
        } else if (source.rfind("uaf:", 0) == 0) {
            std::string rel = source.substr(4);
            int submesh = 0;
            if (const auto h = rel.find_last_of('#'); h != std::string::npos) {
                submesh = std::atoi(rel.c_str() + h + 1);
                rel = rel.substr(0, h);
            }
            if (std::optional<Model> model = uaf::ReadMesh(assetsDir / rel);
                model && submesh >= 0 && submesh < static_cast<int>(model->size()))
                adopt((*model)[static_cast<size_t>(submesh)]);
        }
        cache.emplace(source, geom);
        return geom;
    };

    for (const entt::entity e : reg.view<const Transform, const MeshRef>()) {
        // A mesh opts OUT with a disabled NavmeshInput; explicit dynamic obstacles are
        // NOT baked (they are the runtime dtTileCache obstacle path instead); paint
        // strokes are decals; and an explicitly Dynamic-layer mesh is a mover, not floor.
        if (const NavmeshInput* ni = reg.try_get<const NavmeshInput>(e); ni && !ni->enabled)
            continue;
        if (reg.all_of<NavigationObstacle>(e)) continue;
        if (strokezone::IsStroke(reg, e)) continue;
        if (const SceneLayer* sl = reg.try_get<const SceneLayer>(e);
            sl && sl->kind == SceneKind::Dynamic)
            continue;
        const auto geom = resolve(reg.get<const MeshRef>(e).source);
        if (!geom || geom->first.empty() || geom->second.size() < 3) continue;
        const glm::mat4 world = scene.WorldMatrix(e);
        const int base = static_cast<int>(verts.size() / 3);
        for (const glm::vec3& p : geom->first) {
            const glm::vec3 w = glm::vec3(world * glm::vec4(p, 1.0f));
            verts.push_back(w.x);
            verts.push_back(w.y);
            verts.push_back(w.z);
        }
        const std::vector<i32>& idx = geom->second;
        const int n = static_cast<int>(geom->first.size());
        for (usize i = 0; i + 2 < idx.size(); i += 3) {
            const i32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
            if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) continue;
            tris.push_back(base + a);
            tris.push_back(base + b);
            tris.push_back(base + c);
        }
    }

    // CSG blockout brushes carry no MeshRef (their geometry is derived), so they need their own
    // pass. An ADDITIVE brush contributes its carved solid as static floor/wall; a subtractive brush
    // yields an empty mesh from BuildEntityMesh and drops out. Same opt-out rules as above.
    for (const entt::entity e : reg.view<const BrushComponent>()) {
        if (const NavmeshInput* ni = reg.try_get<const NavmeshInput>(e); ni && !ni->enabled)
            continue;
        if (reg.all_of<NavigationObstacle>(e)) continue;
        if (const SceneLayer* sl = reg.try_get<const SceneLayer>(e);
            sl && sl->kind == SceneKind::Dynamic)
            continue;
        const MeshData md = brush::BuildEntityMesh(scene, e);
        if (md.vertices.empty() || md.indices.size() < 3) continue;
        const glm::mat4 world = scene.WorldMatrix(e);
        const int base = static_cast<int>(verts.size() / 3);
        for (const Vertex& v : md.vertices) {
            const glm::vec3 w = glm::vec3(world * glm::vec4(v.position, 1.0f));
            verts.push_back(w.x);
            verts.push_back(w.y);
            verts.push_back(w.z);
        }
        const int n = static_cast<int>(md.vertices.size());
        for (usize i = 0; i + 2 < md.indices.size(); i += 3) {
            const i32 a = static_cast<i32>(md.indices[i]), b = static_cast<i32>(md.indices[i + 1]),
                      c = static_cast<i32>(md.indices[i + 2]);
            if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) continue;
            tris.push_back(base + a);
            tris.push_back(base + b);
            tris.push_back(base + c);
        }
    }
}

} // namespace

NavBakeResult BakeNavMeshFromGeometry(const std::vector<f32>& verts, const std::vector<i32>& tris,
                                      const NavBuildSettings& settings,
                                      const std::atomic<bool>* cancel) {
    NavBakeResult res;
    const int nverts = static_cast<int>(verts.size() / 3);
    const int ntris = static_cast<int>(tris.size() / 3);
    if (nverts < 3 || ntris < 1) {
        res.message = "no navigation geometry in the scene";
        return res;
    }

    rcContext ctx(false);
    float bmin[3], bmax[3];
    rcCalcBounds(verts.data(), nverts, bmin, bmax);

    const float cs = std::max(0.01f, settings.cellSize);
    const float ch = std::max(0.01f, settings.cellHeight);
    const int tileVoxels = std::max(16, settings.tileVoxels);
    const float tileWorldSize = tileVoxels * cs;

    int gw = 0, gh = 0;
    rcCalcGridSize(bmin, bmax, cs, &gw, &gh);
    const int tw = (gw + tileVoxels - 1) / tileVoxels;
    const int th = (gh + tileVoxels - 1) / tileVoxels;
    if (tw <= 0 || th <= 0) {
        res.message = "degenerate navigation grid";
        return res;
    }
    if (static_cast<u64>(tw) * static_cast<u64>(th) > kNavMaxTiles) {
        res.message = "navigation grid too large (raise cellSize/tileVoxels)";
        return res;
    }

    rcConfig cfg{};
    cfg.cs = cs;
    cfg.ch = ch;
    cfg.walkableSlopeAngle = settings.agentMaxSlopeDeg;
    cfg.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / ch));
    cfg.walkableClimb = static_cast<int>(std::floor(settings.agentMaxClimb / ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = static_cast<int>(rcSqr(8));
    cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = tileVoxels;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = 6.0f * cs;
    cfg.detailSampleMaxError = ch * 1.0f;
    rcVcopy(cfg.bmin, bmin);
    rcVcopy(cfg.bmax, bmax);

    // Bin each triangle into the tiles its (border-expanded) XZ AABB overlaps, so a tile
    // rasterizes only the triangles that can affect it (offline O(tiles) rather than
    // O(tiles*tris)).
    const float border = cfg.borderSize * cs;
    std::vector<std::vector<int>> bins(static_cast<size_t>(tw) * th);
    for (int t = 0; t < ntris; ++t) {
        float mnx = 1e30f, mxx = -1e30f, mnz = 1e30f, mxz = -1e30f;
        for (int k = 0; k < 3; ++k) {
            const int vi = tris[t * 3 + k];
            const float x = verts[vi * 3 + 0], z = verts[vi * 3 + 2];
            mnx = std::min(mnx, x);
            mxx = std::max(mxx, x);
            mnz = std::min(mnz, z);
            mxz = std::max(mxz, z);
        }
        int tx0 = static_cast<int>(std::floor((mnx - border - bmin[0]) / tileWorldSize));
        int tx1 = static_cast<int>(std::floor((mxx + border - bmin[0]) / tileWorldSize));
        int ty0 = static_cast<int>(std::floor((mnz - border - bmin[2]) / tileWorldSize));
        int ty1 = static_cast<int>(std::floor((mxz + border - bmin[2]) / tileWorldSize));
        tx0 = std::clamp(tx0, 0, tw - 1);
        tx1 = std::clamp(tx1, 0, tw - 1);
        ty0 = std::clamp(ty0, 0, th - 1);
        ty1 = std::clamp(ty1, 0, th - 1);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
                bins[static_cast<size_t>(ty) * tw + tx].push_back(t);
    }

    NavTileCompressor comp;
    std::vector<NavTileBuild> columns;
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            if (cancel && cancel->load()) {
                res.message = "cancelled";
                return res;
            }
            NavTileBuild col;
            col.x = tx;
            col.y = ty;
            if (!RasterizeColumn(ctx, verts, nverts, tris, bins[static_cast<size_t>(ty) * tw + tx],
                                 cfg, tx, ty, comp, col))
                continue;
            if (col.layers.empty()) continue;
            columns.push_back(std::move(col));
        }
    }

    if (columns.empty()) {
        res.message = "no walkable navigation tiles produced (check agent/slope settings)";
        return res;
    }

    NavBuildHeader hdr;
    hdr.cellSize = cs;
    hdr.cellHeight = ch;
    hdr.tileVoxels = tileVoxels;
    hdr.tileWorldSize = tileWorldSize;
    hdr.origin[0] = bmin[0];
    hdr.origin[1] = bmin[1];
    hdr.origin[2] = bmin[2];
    hdr.gridMinX = 0;
    hdr.gridMinY = 0;
    hdr.gridMaxX = tw - 1;
    hdr.gridMaxY = th - 1;
    NavAgentProfile prof;
    prof.name = settings.profileName;
    prof.radius = settings.agentRadius;
    prof.height = settings.agentHeight;
    prof.maxClimb = settings.agentMaxClimb;
    prof.maxSlopeDeg = settings.agentMaxSlopeDeg;
    hdr.profiles.push_back(prof);
    hdr.sourceHash = HashGeometry(verts, tris, settings);

    const std::vector<u32> partition = {static_cast<u32>(columns.size())};
    res.bytes = WriteNavMesh(hdr, columns, partition);
    res.ok = !res.bytes.empty();
    res.tileColumns = static_cast<int>(columns.size());
    res.totalTris = ntris;
    res.sourceHash = hdr.sourceHash;
    res.message = res.ok ? "ok" : "navmesh serialisation failed";
    return res;
}

NavBakeResult BakeNavMesh(const Scene& scene, const std::filesystem::path& assetsDir,
                          const NavBuildSettings& settings, const std::atomic<bool>* cancel) {
    std::vector<f32> verts;
    std::vector<i32> tris;
    GatherGeometry(scene, assetsDir, verts, tris);
    return BakeNavMeshFromGeometry(verts, tris, settings, cancel);
}

u64 HashNavInputs(const Scene& scene, const std::filesystem::path& assetsDir,
                  const NavBuildSettings& settings) {
    std::vector<f32> verts;
    std::vector<i32> tris;
    GatherGeometry(scene, assetsDir, verts, tris);
    return HashGeometry(verts, tris, settings);
}

// --- Headless self-test ------------------------------------------------------
namespace {

void AppendQuad(std::vector<f32>& v, std::vector<i32>& t, float x0, float x1, float z0,
                float z1, float y) {
    const int b = static_cast<int>(v.size() / 3);
    const float xs[4] = {x0, x0, x1, x1};
    const float zs[4] = {z0, z1, z1, z0};
    for (int i = 0; i < 4; ++i) {
        v.push_back(xs[i]);
        v.push_back(y);
        v.push_back(zs[i]);
    }
    // (a,b,c),(a,c,d) -> +Y normals (walkable floor).
    t.push_back(b + 0);
    t.push_back(b + 1);
    t.push_back(b + 2);
    t.push_back(b + 0);
    t.push_back(b + 2);
    t.push_back(b + 3);
}

void AppendBox(std::vector<f32>& v, std::vector<i32>& t, const glm::vec3& mn, const glm::vec3& mx) {
    const int b = static_cast<int>(v.size() / 3);
    const glm::vec3 c[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z},
                            {mn.x, mn.y, mx.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
                            {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    for (const glm::vec3& p : c) {
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
    }
    const int f[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 5, 1}, {0, 4, 5},
                          {1, 6, 2}, {1, 5, 6}, {2, 7, 3}, {2, 6, 7}, {3, 4, 0}, {3, 7, 4}};
    for (auto& tri : f) {
        t.push_back(b + tri[0]);
        t.push_back(b + tri[1]);
        t.push_back(b + tri[2]);
    }
}

float PathLength(const std::vector<glm::vec3>& c) {
    float len = 0.0f;
    for (usize i = 1; i < c.size(); ++i) len += glm::distance(c[i - 1], c[i]);
    return len;
}

} // namespace

bool SelfTest() {
    // Synthetic world: a 30x30 m flat floor, plus a wall (x in [-4,4], thin in z, 3 m
    // tall) splitting it so a straight crossing must route around the wall's ends.
    std::vector<f32> verts;
    std::vector<i32> tris;
    AppendQuad(verts, tris, -15.0f, 15.0f, -15.0f, 15.0f, 0.0f);
    AppendBox(verts, tris, glm::vec3(-4.0f, 0.0f, -0.5f), glm::vec3(4.0f, 3.0f, 0.5f));

    NavBuildSettings settings;
    const NavBakeResult bake = BakeNavMeshFromGeometry(verts, tris, settings);
    if (!bake.ok || bake.bytes.empty() || bake.tileColumns <= 0) {
        HBE_ERROR("[nav] SelfTest FAIL: bake produced nothing ('{}').", bake.message);
        return false;
    }
    HBE_INFO("[nav] baked {} tile columns ({} tris).", bake.tileColumns, bake.totalTris);

    // Write + load (exercises the .hbnav round-trip + loose-file streaming reads).
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec) / "hbe_navtest.hbnav";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            HBE_ERROR("[nav] SelfTest FAIL: cannot write temp navmesh.");
            return false;
        }
        f.write(reinterpret_cast<const char*>(bake.bytes.data()),
                static_cast<std::streamsize>(bake.bytes.size()));
    }
    NavMeshData data = LoadNavMesh(tmp);
    if (!data.Valid()) {
        HBE_ERROR("[nav] SelfTest FAIL: reload invalid (status {}).", static_cast<u32>(data.status));
        return false;
    }

    NavMeshManager mgr;
    NavMeshInitParams p;
    p.cellSize = data.cellSize;
    p.cellHeight = data.cellHeight;
    p.tileVoxels = data.tileVoxels;
    p.tileWorldSize = data.tileWorldSize;
    p.origin = glm::vec3(data.origin[0], data.origin[1], data.origin[2]);
    p.agentRadius = data.profiles.front().radius;
    p.agentHeight = data.profiles.front().height;
    p.agentMaxClimb = data.profiles.front().maxClimb;
    p.maxResidentColumns = static_cast<i32>(data.tiles.size()) + 8;
    if (!mgr.Init(p)) {
        HBE_ERROR("[nav] SelfTest FAIL: manager init.");
        return false;
    }
    // Stream every column resident (the test wants the whole navmesh).
    std::vector<u8> blob;
    for (const NavTileRecord& rec : data.tiles) {
        if (!ReadTileBlob(data, rec, blob)) {
            HBE_ERROR("[nav] SelfTest FAIL: blob read for tile ({},{}).", rec.x, rec.y);
            return false;
        }
        mgr.AddColumn(rec.x, rec.y, blob);
    }
    HBE_INFO("[nav] streamed {} resident columns.", mgr.ResidentColumnCount());

    const glm::vec3 ext(2.0f, 6.0f, 2.0f);

    // Test 1 - STATIC bake: crossing the wall must route around it (some corner past the
    // wall end at |x| ~ 4).
    NavPathResult around = mgr.FindPath(glm::vec3(0, 0, -10), glm::vec3(0, 0, 10), ext);
    if (!around.found) {
        HBE_ERROR("[nav] SelfTest FAIL: no path across the wall.");
        return false;
    }
    float maxAbsX = 0.0f;
    for (const glm::vec3& c : around.corners) maxAbsX = std::max(maxAbsX, std::abs(c.x));
    if (maxAbsX < 3.0f) {
        HBE_ERROR("[nav] SelfTest FAIL: path did not route around the wall (maxAbsX={:.2f}).", maxAbsX);
        return false;
    }
    HBE_INFO("[nav] static wall avoided (path bends to x={:.1f}).", maxAbsX);

    // Test 2 - DYNAMIC obstacle: a clear corridor at x=-10, straight -> blocked -> clear.
    const glm::vec3 s(-10, 0, -10), g(-10, 0, 10);
    NavPathResult clearBefore = mgr.FindPath(s, g, ext);
    if (!clearBefore.found) {
        HBE_ERROR("[nav] SelfTest FAIL: no path down the open corridor.");
        return false;
    }
    const float lenBefore = PathLength(clearBefore.corners);

    NavObstacleHandle ob = mgr.AddObstacle(glm::vec3(-10, 0, 0), 3.0f, 3.0f);
    if (!ob.Valid()) {
        HBE_ERROR("[nav] SelfTest FAIL: could not add obstacle.");
        return false;
    }
    mgr.UpdateObstacles(1.0f / 60.0f);
    NavPathResult blocked = mgr.FindPath(s, g, ext);
    const float lenBlocked = PathLength(blocked.corners);
    if (!blocked.found || lenBlocked <= lenBefore + 0.5f) {
        HBE_ERROR("[nav] SelfTest FAIL: obstacle did not lengthen the path ({:.2f} -> {:.2f}).",
                  lenBefore, lenBlocked);
        return false;
    }
    HBE_INFO("[nav] obstacle reroute OK ({:.2f} m -> {:.2f} m).", lenBefore, lenBlocked);

    mgr.RemoveObstacle(ob);
    mgr.UpdateObstacles(1.0f / 60.0f);
    NavPathResult clearAfter = mgr.FindPath(s, g, ext);
    const float lenAfter = PathLength(clearAfter.corners);
    if (!clearAfter.found || lenAfter >= lenBlocked - 0.25f) {
        HBE_ERROR("[nav] SelfTest FAIL: path did not recover after obstacle removal ({:.2f}).", lenAfter);
        return false;
    }
    HBE_INFO("[nav] obstacle removal recovered ({:.2f} m).", lenAfter);

    std::filesystem::remove(tmp, ec);
    HBE_INFO("[nav] SelfTest PASS.");
    return true;
}

} // namespace nav
} // namespace hbe
