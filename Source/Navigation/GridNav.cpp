// Navigation/GridNav.cpp - real-time grid A* + ground query implementation.
#include "Navigation/GridNav.h"

#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/StrokeZone.h" // paint strokes are decals, never nav geometry
#include "Scene/TerrainSystem.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

namespace hbe::nav {

namespace {
u64 CellKey(int cx, int cz) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
}

// 64-bit avalanche, so the fingerprints below can be COMBINED BY ADDITION and stay
// well distributed. Order independence matters: the old fingerprint was an FNV
// CHAIN over entt pool order, and entt's swap-and-pop moves the last element into a
// destroyed slot - so despawning a streamed shard could reorder the accepted
// entities and rebuild byte-identical geometry. A sum cannot see order.
u64 Mix64(u64 v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}
u64 HashF32(f32 f) {
    u32 b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return Mix64(b);
}
u64 HashMat(const glm::mat4& m) {
    u64 h = 0x9e3779b97f4a7c15ull;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) h = Mix64(h ^ HashF32(m[c][r]));
    return h;
}

// 2D (XZ) barycentric height of (x,z) inside triangle t; false if outside.
bool TriHeight(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, f32 x, f32 z, f32& outY) {
    const f32 d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::fabs(d) < 1e-8f) return false;
    const f32 u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
    const f32 v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
    const f32 w = 1.0f - u - v;
    constexpr f32 e = 1e-4f;
    if (u < -e || v < -e || w < -e) return false;
    outY = u * a.y + v * b.y + w * c.y;
    return true;
}

// Squared XZ distance from (px,pz) to a triangle's XZ projection (0 when inside).
// A WALL cannot be tested with TriHeight: a vertical triangle projects to a
// zero-area line in XZ, so the barycentric test essentially never reports a hit and
// vertical geometry would be invisible to a point sample. Distance-to-projection with
// the agent's radius is what actually makes a wall solid.
f32 Dist2XZToTri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, f32 px, f32 pz) {
    const glm::vec2 p(px, pz), A(a.x, a.z), B(b.x, b.z), C(c.x, c.z);
    // Inside test via edge signs (robust for degenerate/near-degenerate triangles: a
    // zero-area triangle simply reports "not inside" and falls through to the edges).
    const f32 d1 = (p.x - A.x) * (B.y - A.y) - (p.y - A.y) * (B.x - A.x);
    const f32 d2 = (p.x - B.x) * (C.y - B.y) - (p.y - B.y) * (C.x - B.x);
    const f32 d3 = (p.x - C.x) * (A.y - C.y) - (p.y - C.y) * (A.x - C.x);
    const bool neg = d1 <= 0.0f && d2 <= 0.0f && d3 <= 0.0f;
    const bool pos = d1 >= 0.0f && d2 >= 0.0f && d3 >= 0.0f;
    if (neg || pos) return 0.0f;
    const auto seg2 = [&](glm::vec2 s, glm::vec2 e) {
        const glm::vec2 d = e - s;
        const f32 len2 = glm::dot(d, d);
        const f32 t = len2 > 1e-12f ? glm::clamp(glm::dot(p - s, d) / len2, 0.0f, 1.0f) : 0.0f;
        const glm::vec2 q = s + d * t;
        return glm::dot(p - q, p - q);
    };
    return std::min({seg2(A, B), seg2(B, C), seg2(C, A)});
}

// --- What counts as nav geometry ----------------------------------------------
// ONE predicate, shared by the collector, the streamed-geometry sync and
// EnsureBuilt's fingerprint, so a change to a mesh the collector uses cannot fail
// to trigger a rebuild.
enum class NavGeometryFilter { Static, Input, All };

// STREAMED ENTITIES DO NOT GET A VOTE on which filter is chosen. The filter has to
// be derived from the ALWAYS-RESIDENT set, or a shard spawning can empty the navmesh
// for the whole level: a level with no SceneLayer components (normal) and one
// NavmeshInput prop inside a streamed tag would flip from filter All (untagged ground
// collected) to filter Input (untagged ground rejected) the moment that shard spawned.
// The Static branch flips the same way when the only Static-layer meshes live in
// shards. Shard CONTENT is nav geometry now (see SyncStreamedGeometry); shard content
// still does not get to redefine the predicate.
NavGeometryFilter ChooseNavFilter(const entt::registry& reg) {
    for (const entt::entity e : reg.view<const SceneLayer>())
        if (!reg.all_of<StreamShard>(e) &&
            reg.get<const SceneLayer>(e).kind == SceneKind::Static)
            return NavGeometryFilter::Static;
    for (const entt::entity e : reg.view<const NavmeshInput>())
        if (!reg.all_of<StreamShard>(e)) return NavGeometryFilter::Input;
    return NavGeometryFilter::All;
}

// The layer/tag half of the predicate - identical for resident and streamed meshes.
bool NavFilterAccepts(const entt::registry& reg, entt::entity e, NavGeometryFilter filter) {
    // 3D PAINT STROKES ARE DECALS, NOT COLLISION. A stroke is an ordinary lit mesh
    // entity carrying Transform + MeshRef - exactly the view collected below - so a
    // stroke painted on a wall used to BE a wall and one on the floor a step, in
    // every filter mode. Rejected here, in the one shared predicate, so the
    // collector, the streamed-geometry sync and the rebuild fingerprint all agree.
    //
    // Deliberately NOT done by stamping NavmeshInput{enabled=false} on strokes: a
    // level whose only NavmeshInput components sat on strokes would flip
    // ChooseNavFilter to the Input filter and reject the ground with them.
    if (strokezone::IsStroke(reg, e)) return false;
    if (filter == NavGeometryFilter::Static) {
        // UNTAGGED == STATIC. A level is ONE scene file now, so an object's layer rides
        // on the object; requiring an explicit tag here would silently drop every object
        // an artist never touched in the Inspector out of the navmesh. This matches the
        // deleted build-time split's own default and the Inspector combo, which already
        // displays untagged as Static.
        const SceneLayer* sl = reg.try_get<SceneLayer>(e);
        if (sl && sl->kind != SceneKind::Static) return false;
        const NavmeshInput* ni = reg.try_get<NavmeshInput>(e);
        if (ni && !ni->enabled) return false; // explicit opt-out of a static mesh
    } else if (filter == NavGeometryFilter::Input) {
        const NavmeshInput* ni = reg.try_get<NavmeshInput>(e);
        if (!ni || !ni->enabled) return false;
    }
    return true;
}

// The ALWAYS-RESIDENT static set (what a full Rebuild collects). Streamed holders are
// excluded HERE - not because they are not nav geometry (they are, and agents used to
// walk straight through spawned props) but because they enter through the incremental
// path instead, so a spawn cannot trigger a full rebuild.
bool IsNavGeometry(const entt::registry& reg, entt::entity e, NavGeometryFilter filter) {
    if (reg.all_of<StreamShard>(e)) return false;
    return NavFilterAccepts(reg, e, filter);
}

// Inclusive bucket range of a triangle's XZ AABB. `fullyInside` reports whether the
// UNCLAMPED range fit the grid - an incremental insert that does not fit asks for a
// re-index instead of silently dropping the triangle.
bool BucketRange(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, glm::vec2 gridMin,
                 f32 bucket, int gw, int gh, int& ix0, int& ix1, int& iz0, int& iz1,
                 bool& fullyInside) {
    fullyInside = false;
    if (gw <= 0 || gh <= 0 || bucket <= 0.0f) return false;
    const f32 x0 = std::min({a.x, b.x, c.x}), x1 = std::max({a.x, b.x, c.x});
    const f32 z0 = std::min({a.z, b.z, c.z}), z1 = std::max({a.z, b.z, c.z});
    const int rx0 = static_cast<int>(std::floor((x0 - gridMin.x) / bucket));
    const int rx1 = static_cast<int>(std::floor((x1 - gridMin.x) / bucket));
    const int rz0 = static_cast<int>(std::floor((z0 - gridMin.y) / bucket));
    const int rz1 = static_cast<int>(std::floor((z1 - gridMin.y) / bucket));
    fullyInside = rx0 >= 0 && rz0 >= 0 && rx1 < gw && rz1 < gh;
    ix0 = std::max(0, rx0);
    ix1 = std::min(gw - 1, rx1);
    iz0 = std::max(0, rz0);
    iz1 = std::min(gh - 1, rz1);
    return ix0 <= ix1 && iz0 <= iz1; // false = entirely outside the grid
}
} // namespace

void GridNav::SetParams(const GridNavParams& p) {
    params_ = p;
    cosSlope_ = std::cos(glm::radians(params_.maxSlopeDeg));
}

void GridNav::Clear() {
    tris_.clear();
    freeTris_.clear();
    liveTris_ = 0;
    staticTris_ = 0;
    buckets_.clear();
    terrains_.clear();
    dyn_.clear();
    gw_ = gh_ = 0;
    cheapSig_ = 0;
    heavySig_ = 0;
    heavyValid_ = false;
    streamSig_ = 0;
    streamSigValid_ = false;
    streamPending_ = false;
    // meshCache_ deliberately SURVIVES. It used to be dropped here, which made every
    // full Rebuild re-read and re-decode every nav mesh from disk (uaf::ReadMesh) on
    // the main thread - and Rebuild fires on any change to the static entity set, which
    // in a shipping frame includes an enemy spawning or dying mid-combat. Use
    // DropGeometryCache() for the case the drop was actually for (asset reimport).
}

bool GridNav::HasStreamedShard(u32 shardIndex) const {
    for (const DynBlock& b : dyn_)
        if (b.shard == shardIndex) return true;
    return false;
}

const std::vector<int>& GridNav::BucketAt(f32 x, f32 z) const {
    static const std::vector<int> empty;
    if (gw_ <= 0 || gh_ <= 0) return empty;
    const int ix = static_cast<int>(std::floor((x - gridMin_.x) / bucket_));
    const int iz = static_cast<int>(std::floor((z - gridMin_.y) / bucket_));
    if (ix < 0 || iz < 0 || ix >= gw_ || iz >= gh_) return empty;
    return buckets_[static_cast<size_t>(iz) * gw_ + ix];
}

// --- Geometry resolution (cached) --------------------------------------------
const GridNav::NavGeom* GridNav::ResolveGeom(const std::filesystem::path& assetsDir,
                                             const std::string& source) {
    if (source.empty()) return nullptr;
    const auto it = meshCache_.find(source);
    if (it != meshCache_.end()) return it->second.get();

    // The cache outlives Clear/Rebuild (see Clear) and is dropped explicitly by
    // DropGeometryCache when an asset is reimported.
    std::shared_ptr<NavGeom> g;
    const auto adopt = [&](const MeshData& md) {
        g = std::make_shared<NavGeom>();
        g->pos.reserve(md.vertices.size());
        for (const Vertex& v : md.vertices) g->pos.push_back(v.position);
        g->idx.reserve(md.indices.size());
        for (const u32 i : md.indices) g->idx.push_back(static_cast<i32>(i));
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
            model && submesh >= 0 && submesh < static_cast<int>(model->size())) {
            adopt((*model)[static_cast<size_t>(submesh)]);
        }
    }
    // A miss is cached as null too: a broken source must not be re-read once per
    // entity, per rebuild (six submesh entities on one file was six full decodes).
    const auto [ins, ok] = meshCache_.emplace(source, std::move(g));
    return ins->second.get();
}

void GridNav::AppendTris(const NavGeom& g, const glm::mat4& world, std::vector<Tri>& out) const {
    if (g.pos.empty() || g.idx.size() < 3) return;
    std::vector<glm::vec3> wp(g.pos.size());
    for (size_t i = 0; i < g.pos.size(); ++i) wp[i] = glm::vec3(world * glm::vec4(g.pos[i], 1.0f));
    const int n = static_cast<int>(wp.size());
    for (size_t i = 0; i + 2 < g.idx.size(); i += 3) {
        const i32 i0 = g.idx[i], i1 = g.idx[i + 1], i2 = g.idx[i + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= n || i1 >= n || i2 >= n) continue;
        Tri t;
        t.a = wp[static_cast<size_t>(i0)];
        t.b = wp[static_cast<size_t>(i1)];
        t.c = wp[static_cast<size_t>(i2)];
        const glm::vec3 nrm = glm::cross(t.b - t.a, t.c - t.a);
        const f32 len = glm::length(nrm);
        t.walkable = len > 1e-6f && (std::fabs(nrm.y) / len) >= cosSlope_;
        t.live = true;
        out.push_back(t);
    }
}

// --- Terrain surfaces (borrowed, refreshed every frame) ----------------------
void GridNav::RefreshTerrains(const Scene& scene) {
    terrains_.clear();
    const entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<const TerrainComponent>()) {
        const TerrainComponent& t = reg.get<const TerrainComponent>(e);
        TerrainSurface ts;
        ts.comp = &t;
        ts.entity = e;
        ts.world = scene.WorldMatrix(e);
        const glm::vec3 c0(ts.world[0]), c1(ts.world[1]), c2(ts.world[2]);
        const f32 sx = glm::length(c0), sy = glm::length(c1), sz = glm::length(c2);
        if (sx < 1e-6f || sy < 1e-6f || sz < 1e-6f) continue; // degenerate transform
        // A TILTED heightfield is not a height function: a vertical line through it can
        // hit the surface more than once, and GroundAt(x,z) has no answer. Translation,
        // scale and YAW are all fine; anything else is sampled UNROTATED (which at least
        // keeps the ground where the author put it) and said out loud once.
        if (std::fabs(c1.y / sy) < 0.999f) {
            if (!warnedTilt_) {
                warnedTilt_ = true;
                HBE_WARN("GridNav: terrain is rotated off vertical; a tilted heightfield is "
                         "not a height function, so navigation samples it unrotated. Keep "
                         "terrain rotation to yaw only.");
            }
            ts.world = glm::translate(glm::mat4(1.0f), glm::vec3(ts.world[3])) *
                       glm::scale(glm::mat4(1.0f), glm::vec3(sx, sy, sz));
        }
        ts.invWorld = glm::inverse(ts.world);
        ts.halfExtent = terrain::ExtentXZ(t) * 0.5f;
        // Local dh/dx is a rise per LOCAL unit; the world gradient scales by the Y
        // scale over the horizontal scale. (With yaw AND non-uniform XZ scale this is
        // an approximation - the exact value would rotate the gradient first.)
        ts.gradScaleX = sy / sx;
        ts.gradScaleZ = sy / sz;
        if (ts.halfExtent <= 0.0f) continue;
        terrains_.push_back(ts);
    }
}

// --- Streamed geometry (incremental, no rebuild) ------------------------------
void GridNav::SyncStreamedGeometry(const Scene& scene, const std::filesystem::path& assetsDir) {
    const entt::registry& reg = scene.Registry();

    // STEP 1 - the cheap gate. Hash only the resident streamed ENTITY SET (id + shard
    // index): no string hash, no transform, no map insert, no allocation. This used to
    // rebuild a per-entity content fingerprint into a fresh unordered_map every frame,
    // measured at 0.114 ms with 2000 streamed entities - 28% of the frame's entire CPU
    // headroom, spent re-deriving something that only changes when the streamer spawns
    // or despawns a shard. See the streamSig_ note in GridNav.h for what that trades.
    u64 sig = Mix64(0xb1a5edull);
    u32 residentEnts = 0;
    for (const entt::entity e : reg.view<const StreamShard>()) {
        sig += Mix64((static_cast<u64>(reg.get<const StreamShard>(e).index) << 32) ^
                     static_cast<u64>(entt::to_integral(e)));
        ++residentEnts;
    }
    sig += Mix64(residentEnts);
    // Anything still pending from a previous frame's budget must keep draining, so the
    // gate only closes once the index actually agrees with the world.
    if (streamSigValid_ && sig == streamSig_ && !streamPending_) return;

    // STEP 2 - the world changed (or we still owe work). Now pay for the real per-shard
    // content fingerprint.
    std::unordered_map<u32, u64> live;
    const NavGeometryFilter filter = ChooseNavFilter(reg);
    for (const entt::entity e : reg.view<const Transform, const MeshRef, const StreamShard>()) {
        if (!NavFilterAccepts(reg, e, filter)) continue;
        const MeshRef& mr = reg.get<const MeshRef>(e);
        if (mr.source.empty()) continue;
        const Transform& tf = reg.get<const Transform>(e);
        u64 h = Mix64(static_cast<u64>(entt::to_integral(e)));
        h ^= Mix64(std::hash<std::string>{}(mr.source));
        h ^= HashF32(tf.position.x) ^ Mix64(HashF32(tf.position.y)) ^ HashF32(tf.position.z);
        h ^= Mix64(HashF32(tf.scale.x) ^ HashF32(tf.scale.y) ^ HashF32(tf.scale.z));
        h ^= Mix64(HashF32(tf.rotation.x) ^ HashF32(tf.rotation.y) ^ HashF32(tf.rotation.z) ^
                   HashF32(tf.rotation.w));
        live[reg.get<const StreamShard>(e).index] += Mix64(h);
    }

    // Drop blocks whose shard despawned, or whose content changed (re-added below).
    for (size_t i = dyn_.size(); i-- > 0;) {
        const auto it = live.find(dyn_[i].shard);
        if (it != live.end() && it->second == dyn_[i].sig) {
            live.erase(it); // already indexed and unchanged
            continue;
        }
        RemoveBlock(dyn_[i]);
        dyn_.erase(dyn_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    streamSig_ = sig;
    streamSigValid_ = true;
    streamPending_ = false;
    if (live.empty()) return;

    // DETERMINISTIC ORDER. unordered_map iteration order varies between runs, so with a
    // budget below the pending count, WHICH shards got indexed this frame - and
    // therefore which tris_ slots their triangles occupied - used to differ run to run.
    // GroundAt walks bucket contents in slot order, so coincident surfaces could resolve
    // differently in an otherwise identical scene. Sort before applying the budget.
    std::vector<u32> order;
    order.reserve(live.size());
    for (const auto& [shard, unused] : live) order.push_back(shard);
    std::sort(order.begin(), order.end());

    // BUDGETED, like the streamer's own one-shard-per-frame finalize budget: a corner
    // turn that makes several shards resident in the same frame must not stack their
    // insert cost. Whatever is left over is picked up next frame (streamPending_ keeps
    // the cheap gate open until it is).
    int budget = 2;
    for (const u32 shard : order) {
        if (budget-- <= 0) {
            streamPending_ = true;
            break;
        }
        DynBlock block;
        block.shard = shard;
        block.sig = live[shard];
        for (const entt::entity e : reg.view<const Transform, const MeshRef, const StreamShard>()) {
            if (reg.get<const StreamShard>(e).index != shard) continue;
            if (!NavFilterAccepts(reg, e, filter)) continue;
            const NavGeom* g = ResolveGeom(assetsDir, reg.get<const MeshRef>(e).source);
            if (g) AppendTris(*g, scene.WorldMatrix(e), block.tris);
        }
        const usize count = block.tris.size();
        AddBlock(std::move(block));
        HBE_TRACE("GridNav: indexed streamed shard {} ({} triangles) - no rebuild.", shard,
                  static_cast<u32>(count));
    }
}

// --- Index maintenance -------------------------------------------------------
void GridNav::BuildBuckets() {
    buckets_.clear();
    gw_ = gh_ = 0;

    glm::vec2 mn(1e30f), mx(-1e30f);
    bool any = false;
    const auto expand = [&](const glm::vec3& p) {
        mn.x = std::min(mn.x, p.x);
        mn.y = std::min(mn.y, p.z);
        mx.x = std::max(mx.x, p.x);
        mx.y = std::max(mx.y, p.z);
        any = true;
    };
    for (const Tri& t : tris_) {
        if (!t.live) continue;
        expand(t.a);
        expand(t.b);
        expand(t.c);
    }
    // Terrain is NOT bucketed (it is sampled analytically) but its footprint still
    // sizes the grid, so streamed props standing on terrain land in real buckets
    // instead of falling outside the authored static bounds.
    for (const TerrainSurface& ts : terrains_) {
        for (const f32 sx : {-1.0f, 1.0f})
            for (const f32 sz : {-1.0f, 1.0f})
                expand(glm::vec3(ts.world * glm::vec4(sx * ts.halfExtent, 0.0f,
                                                      sz * ts.halfExtent, 1.0f)));
    }
    if (!any) return;

    // 2D XZ bucket index for height queries. Cap the dimensions (grow the bucket
    // for sprawling worlds) so memory stays bounded.
    gridMin_ = mn - glm::vec2(0.5f);
    const glm::vec2 span = (mx - mn) + glm::vec2(1.0f);
    bucket_ = std::max(2.0f, params_.cellSize * 4.0f);
    gw_ = std::max(1, static_cast<int>(std::ceil(span.x / bucket_)));
    gh_ = std::max(1, static_cast<int>(std::ceil(span.y / bucket_)));
    constexpr int kMaxDim = 1024;
    while (gw_ > kMaxDim || gh_ > kMaxDim) {
        bucket_ *= 2.0f;
        gw_ = std::max(1, static_cast<int>(std::ceil(span.x / bucket_)));
        gh_ = std::max(1, static_cast<int>(std::ceil(span.y / bucket_)));
    }
    buckets_.assign(static_cast<size_t>(gw_) * gh_, {});
    for (int ti = 0; ti < static_cast<int>(tris_.size()); ++ti) {
        if (!tris_[ti].live) continue;
        const Tri& t = tris_[ti];
        int ix0, ix1, iz0, iz1;
        bool inside = false;
        if (!BucketRange(t.a, t.b, t.c, gridMin_, bucket_, gw_, gh_, ix0, ix1, iz0, iz1, inside))
            continue;
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix)
                buckets_[static_cast<size_t>(iz) * gw_ + ix].push_back(ti);
    }
}

bool GridNav::IndexTri(int slot) {
    const Tri& t = tris_[static_cast<size_t>(slot)];
    int ix0, ix1, iz0, iz1;
    bool inside = false;
    const bool hit = BucketRange(t.a, t.b, t.c, gridMin_, bucket_, gw_, gh_, ix0, ix1, iz0, iz1,
                                 inside);
    if (!hit || !inside) return false; // needs a re-index (grid missing or too small)
    for (int iz = iz0; iz <= iz1; ++iz)
        for (int ix = ix0; ix <= ix1; ++ix)
            buckets_[static_cast<size_t>(iz) * gw_ + ix].push_back(slot);
    return true;
}

void GridNav::UnindexTri(int slot) {
    const Tri& t = tris_[static_cast<size_t>(slot)];
    int ix0, ix1, iz0, iz1;
    bool inside = false;
    // The CLAMPED range is what was inserted (by IndexTri when it fit, by BuildBuckets
    // otherwise), so removal always visits exactly the buckets that hold this slot.
    if (!BucketRange(t.a, t.b, t.c, gridMin_, bucket_, gw_, gh_, ix0, ix1, iz0, iz1, inside))
        return;
    for (int iz = iz0; iz <= iz1; ++iz)
        for (int ix = ix0; ix <= ix1; ++ix) {
            std::vector<int>& b = buckets_[static_cast<size_t>(iz) * gw_ + ix];
            b.erase(std::remove(b.begin(), b.end(), slot), b.end());
        }
}

void GridNav::AddBlock(DynBlock&& block) {
    dyn_.push_back(std::move(block));
    DynBlock& b = dyn_.back();
    b.slots.clear();
    b.slots.reserve(b.tris.size());
    bool needReindex = false;
    for (const Tri& t : b.tris) {
        int slot;
        if (!freeTris_.empty()) {
            slot = freeTris_.back();
            freeTris_.pop_back();
            tris_[static_cast<size_t>(slot)] = t;
        } else {
            slot = static_cast<int>(tris_.size());
            tris_.push_back(t);
        }
        b.slots.push_back(slot);
        ++liveTris_;
        if (!IndexTri(slot)) needReindex = true;
    }
    // A shard beyond the authored bounds (or the first geometry in a world that had
    // none) resizes the index. That is a re-INDEX, not a rebuild: no gather, no disk,
    // no asset decode - it only re-sorts triangles it already has.
    if (needReindex) BuildBuckets();
}

void GridNav::RemoveBlock(DynBlock& block) {
    for (const int slot : block.slots) {
        UnindexTri(slot);
        Tri& t = tris_[static_cast<size_t>(slot)];
        t.live = false;
        t.walkable = false;
        freeTris_.push_back(slot);
        --liveTris_;
    }
    block.slots.clear();
}

// --- Benchmark hooks (--navbench) --------------------------------------------
void GridNav::BenchStreamPoll(const Scene& scene, const std::filesystem::path& assetsDir,
                              bool forceFullFingerprint) {
    if (forceFullFingerprint) {
        // Defeat the cheap gate so the call pays the full per-entity content fingerprint
        // (string hash + transform hash + map insert), which is what ran EVERY frame
        // before. Nothing is actually re-indexed: the per-shard sigs still match.
        streamSigValid_ = false;
    }
    SyncStreamedGeometry(scene, assetsDir);
}

bool GridNav::BenchLinearBlocked(const std::vector<GridObstacle>& obs, f32 pad, f32 x, f32 z) {
    for (const GridObstacle& o : obs) {
        const f32 dx = x - o.pos.x, dz = z - o.pos.z;
        const f32 r = o.radius + pad;
        if (dx * dx + dz * dz < r * r) return true;
    }
    return false;
}

void GridNav::Rebuild(const Scene& scene, const std::filesystem::path& assetsDir) {
    // Streamed blocks SURVIVE a rebuild: they are owned world-space triangles, so
    // re-indexing them costs nothing but a memcpy - re-deriving them would re-read
    // every shard's assets.
    std::vector<DynBlock> keep = std::move(dyn_);
    // Clear() zeroes the fingerprints, which made the editor's explicit "Rebuild Grid"
    // button cost TWO full rebuilds: the button's, then EnsureBuilt's on the next frame
    // because 0 never matches the live fingerprint. Preserve them - EnsureBuilt
    // overwrites them with the values it actually computed whenever IT is the one asking
    // for the rebuild.
    const u64 keepCheap = cheapSig_, keepHeavy = heavySig_;
    const bool keepHeavyValid = heavyValid_;
    Clear();
    cheapSig_ = keepCheap;
    heavySig_ = keepHeavy;
    heavyValid_ = keepHeavyValid;
    RefreshTerrains(scene);

    const entt::registry& reg = scene.Registry();
    const NavGeometryFilter filter = ChooseNavFilter(reg);
    for (const entt::entity e : reg.view<const Transform, const MeshRef>()) {
        if (!IsNavGeometry(reg, e, filter)) continue;
        if (const NavGeom* g = ResolveGeom(assetsDir, reg.get<const MeshRef>(e).source))
            AppendTris(*g, scene.WorldMatrix(e), tris_);
    }
    staticTris_ = static_cast<int>(tris_.size());
    liveTris_ = staticTris_;

    dyn_ = std::move(keep);
    for (DynBlock& b : dyn_) {
        b.slots.clear();
        b.slots.reserve(b.tris.size());
        for (const Tri& t : b.tris) {
            b.slots.push_back(static_cast<int>(tris_.size()));
            tris_.push_back(t);
            ++liveTris_;
        }
    }
    BuildBuckets();
    ++rebuilds_;
    HBE_INFO("GridNav: built height index from {} static triangles ({}x{} buckets), {} analytic "
             "terrain surface(s), {} streamed block(s).",
             staticTris_, gw_, gh_, static_cast<u32>(terrains_.size()),
             static_cast<u32>(dyn_.size()));
}

void GridNav::ScanStatic(const Scene& scene, bool withHeavy, u64& outCheap, u64& outHeavy,
                         u32& outCount) {
    const entt::registry& reg = scene.Registry();
    u64 cheap = Mix64(0x5eed5eed5eed5eedull);
    u64 heavy = Mix64(0xbeefcafed00dfaceull);
    u32 count = 0;
    const NavGeometryFilter filter = ChooseNavFilter(reg);
    // view<Transform, MeshRef> - the SAME set Rebuild collects. Hashing the wider
    // view<MeshRef> would count entities the collector never reads.
    for (const entt::entity e : reg.view<const Transform, const MeshRef>()) {
        if (!IsNavGeometry(reg, e, filter)) continue;
        ++count;
        cheap += Mix64(static_cast<u64>(entt::to_integral(e)));
        if (!withHeavy) continue;
        // WORLD MATRIX AND MESH SOURCE. With only the entity id, moving a static wall
        // 10 m with the gizmo (or repointing its MeshRef at a different model) left the
        // entity set and the count unchanged, so the fingerprint matched, no rebuild
        // fired, and tris_ kept the wall's OLD triangles - agents pathing through the
        // new wall and around empty air, permanently, until a level load.
        heavy += Mix64(static_cast<u64>(entt::to_integral(e))) ^ HashMat(scene.WorldMatrix(e)) ^
                 Mix64(std::hash<std::string>{}(reg.get<const MeshRef>(e).source));
    }
    cheap += Mix64(count);
    // Report what the predicate actually resolved to (the editor panel reads this).
    source_ = filter == NavGeometryFilter::Static  ? NavSource::StaticLayer
              : filter == NavGeometryFilter::Input ? NavSource::NavmeshInputTag
                                                   : NavSource::AllMeshes;
    acceptedMeshes_ = static_cast<int>(count);
    // Terrain LAYOUT is cheap-tier: there are a handful of terrains, never thousands,
    // and a terrain being moved or resized must not wait for a heavy frame.
    for (const TerrainSurface& ts : terrains_) {
        // Keyed by ENTITY, not by `ts.comp`. That was a raw address into entt component
        // storage, and entt's swap-and-pop relocates components - so destroying an
        // unrelated entity could move the terrain component and spuriously fire a full
        // rebuild (which, before the cache fix, re-read every static mesh from disk).
        cheap += Mix64(static_cast<u64>(entt::to_integral(ts.entity))) ^
                 Mix64(ts.comp->GridN()) ^ HashF32(ts.halfExtent) ^ HashMat(ts.world);
    }
    cheap += Mix64(HashF32(params_.cellSize) ^ Mix64(HashF32(params_.maxSlopeDeg)) ^
                   Mix64(HashF32(params_.maxStep)) ^ Mix64(HashF32(params_.climb)) ^
                   Mix64(HashF32(params_.agentRadius)) ^ Mix64(HashF32(params_.agentHeight)));
    outCheap = cheap;
    outHeavy = heavy;
    outCount = count;
}

bool GridNav::EnsureBuilt(const Scene& scene, const std::filesystem::path& assetsDir) {
    // 1. Re-BORROW the terrain surfaces every frame. This is the whole reason a sculpt
    //    stroke needs no rebuild: nav reads the same `heights` array the brush writes,
    //    so the only per-frame work is refreshing a pointer and a world matrix. It also
    //    means the borrow must not outlive the frame - EnsureHeights can reallocate.
    RefreshTerrains(scene);

    // 2. Fingerprint the ALWAYS-RESIDENT static set + the terrain LAYOUT + the params,
    //    so a level load, a terrain being added/moved/resized, or a slope/cell-size
    //    change rebuilds - and a terrain SCULPT does not (heights are deliberately not
    //    hashed). Streamed holders are excluded: they come in through step 3.
    //    Two tiers - see cheapSig_/heavySig_ in the header for why and what it costs.
    ++fpFrame_;
    bool heavyNow = !heavyValid_ || (fpFrame_ % kHeavyFingerprintPeriod) == 0;
    u64 cheap = 0, heavy = 0;
    u32 count = 0;
    ScanStatic(scene, heavyNow, cheap, heavy, count);

    bool changed = cheap != cheapSig_ || (!Ready() && count > 0);
    if (!changed && heavyNow && heavy != heavySig_) changed = true;
    // A cheap-tier change on a non-heavy frame still has to leave a CORRECT heavy sig
    // behind, or the next heavy frame would compare the new geometry against the old
    // hash and rebuild a second time for nothing. Re-scan (we are rebuilding anyway).
    if (changed && !heavyNow) {
        ScanStatic(scene, true, cheap, heavy, count);
        heavyNow = true;
    }
    if (changed) Rebuild(scene, assetsDir);
    cheapSig_ = cheap;
    if (heavyNow) {
        heavySig_ = heavy;
        heavyValid_ = true;
    }

    // 3. Diff the streamed shards and insert/remove their geometry incrementally.
    SyncStreamedGeometry(scene, assetsDir);
    return Ready();
}

std::optional<f32> GridNav::GroundAt(f32 x, f32 z, f32 nearY) const {
    return Ground(x, z, nearY, /*requireClearance=*/false);
}

std::optional<f32> GridNav::Ground(f32 x, f32 z, f32 nearY, bool requireClearance) const {
    f32 best = 0.0f;
    f32 bestDist = params_.climb;
    bool found = false;
    const auto consider = [&](f32 y) {
        const f32 d = std::fabs(y - nearY);
        if (d <= params_.climb && (!found || d < bestDist)) { // closest surface to nearY
            best = y;
            bestDist = d;
            found = true;
        }
    };

    const std::vector<int>& bucket = BucketAt(x, z);
    for (const int ti : bucket) {
        const Tri& t = tris_[static_cast<size_t>(ti)];
        if (!t.walkable) continue;
        f32 y;
        if (!TriHeight(t.a, t.b, t.c, x, z, y)) continue;
        consider(y);
    }

    // ANALYTIC TERRAIN. Consulted even when there are no triangles at all (a terrain-
    // only world is a valid world) and even outside the mesh bucket grid.
    for (const TerrainSurface& ts : terrains_) {
        const glm::vec4 lp = ts.invWorld * glm::vec4(x, nearY, z, 1.0f);
        f32 h = 0.0f, dhdx = 0.0f, dhdz = 0.0f;
        bool hole = false;
        // Outside the terrain footprint is NOT ground: the heightmap is edge-clamped,
        // so sampling past the border would invent an infinite skirt of walkable floor.
        if (!terrain::SampleSurface(*ts.comp, lp.x, lp.z, h, dhdx, dhdz, hole)) continue;
        if (hole) continue; // a painted hole is a hole in the ground, for AI as well
        const f32 gx = dhdx * ts.gradScaleX, gz = dhdz * ts.gradScaleZ;
        // Slope from the SAME four samples that produced the height: the cosine of the
        // surface normal against +Y is 1/sqrt(gx^2+gz^2+1).
        if (1.0f / std::sqrt(gx * gx + gz * gz + 1.0f) < cosSlope_) continue; // a cliff/wall
        consider(ts.world[0].y * lp.x + ts.world[1].y * h + ts.world[2].y * lp.z + ts.world[3].y);
    }
    if (!found) return std::nullopt;

    // --- CLEARANCE: non-walkable triangles are BLOCKERS, not just "not ground" -----
    // Nothing used to read a non-walkable triangle at all, so a prop only blocked an
    // agent through the step-height rule against its walkable TOP face - and `consider`
    // discards any surface farther than `climb` (2 m) from the reference height. A 3 m
    // container on flat ground was therefore completely invisible: its walls were
    // skipped as non-walkable, its roof was out of climb range, the ground underneath
    // was walkable, and the agent walked straight through it. That is verbatim the bug
    // the streamed-geometry work claimed to fix, and it was fixed only for props between
    // maxStep (0.5 m) and climb (2 m) tall.
    //
    // A wall is rejected by its XZ DISTANCE, not by a point-in-triangle test - see
    // Dist2XZToTri. The vertical band tested is [ground + 0.1, ground + agentHeight],
    // so an agent can still stand ON a surface whose own rim triangles are steep (their
    // top is at the ground, below the band).
    if (requireClearance) {
        const f32 lo = best + 0.1f, hi = best + params_.agentHeight;
        const f32 clear2 = params_.agentRadius * params_.agentRadius;
        for (const int ti : bucket) {
            const Tri& t = tris_[static_cast<size_t>(ti)];
            if (t.walkable || !t.live) continue;
            // Vertical-extent reject first: it is three compares and it discards almost
            // every blocker candidate (roofs below, gantries above) before any distance
            // work happens.
            const f32 tHi = std::max({t.a.y, t.b.y, t.c.y});
            if (tHi < lo) continue;
            const f32 tLo = std::min({t.a.y, t.b.y, t.c.y});
            if (tLo > hi) continue;
            if (Dist2XZToTri(t.a, t.b, t.c, x, z) <= clear2) return std::nullopt;
        }
    }
    return best;
}

// --- Obstacle index ----------------------------------------------------------
void GridNav::ObstacleGrid::Build(const std::vector<GridObstacle>& obs, f32 pad) {
    padded.clear();
    cells.clear();
    w = h = 0;
    empty = true;
    if (obs.empty()) return; // the common case: no allocation, Blocked() is a bool test

    padded.reserve(obs.size());
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const GridObstacle& o : obs) {
        GridObstacle p = o;
        p.radius = o.radius + pad;
        if (p.radius <= 0.0f) continue;
        padded.push_back(p);
        lo = glm::min(lo, glm::vec2(p.pos.x - p.radius, p.pos.z - p.radius));
        hi = glm::max(hi, glm::vec2(p.pos.x + p.radius, p.pos.z + p.radius));
    }
    if (padded.empty()) return;
    empty = false;

    mn = lo;
    const glm::vec2 span = hi - lo;
    cell = std::max(1.0f, std::sqrt(span.x * span.y / static_cast<f32>(padded.size()) + 1.0f));
    constexpr int kMaxDim = 128; // keeps the grid bounded for a sprawling obstacle set
    w = std::clamp(static_cast<int>(std::ceil(span.x / cell)) + 1, 1, kMaxDim);
    h = std::clamp(static_cast<int>(std::ceil(span.y / cell)) + 1, 1, kMaxDim);
    cell = std::max({cell, span.x / static_cast<f32>(w), span.y / static_cast<f32>(h), 1e-3f});
    cells.assign(static_cast<size_t>(w) * h, {});
    for (int i = 0; i < static_cast<int>(padded.size()); ++i) {
        const GridObstacle& o = padded[static_cast<size_t>(i)];
        const int x0 = std::clamp(static_cast<int>((o.pos.x - o.radius - mn.x) / cell), 0, w - 1);
        const int x1 = std::clamp(static_cast<int>((o.pos.x + o.radius - mn.x) / cell), 0, w - 1);
        const int z0 = std::clamp(static_cast<int>((o.pos.z - o.radius - mn.y) / cell), 0, h - 1);
        const int z1 = std::clamp(static_cast<int>((o.pos.z + o.radius - mn.y) / cell), 0, h - 1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) cells[static_cast<size_t>(z) * w + x].push_back(i);
    }
}

bool GridNav::ObstacleGrid::Blocked(f32 x, f32 z) const {
    if (empty) return false;
    const int ix = static_cast<int>((x - mn.x) / cell);
    const int iz = static_cast<int>((z - mn.y) / cell);
    if (ix < 0 || iz < 0 || ix >= w || iz >= h) return false; // outside every obstacle's box
    for (const int i : cells[static_cast<size_t>(iz) * w + ix]) {
        const GridObstacle& o = padded[static_cast<size_t>(i)];
        const f32 dx = x - o.pos.x, dz = z - o.pos.z;
        if (dx * dx + dz * dz < o.radius * o.radius) return true;
    }
    return false;
}

bool GridNav::CellWalkable(int cx, int cz, f32 refY, const ObstacleGrid& obs, f32& outGroundY,
                           bool skipClearance) const {
    const f32 cs = params_.cellSize;
    const f32 wx = cx * cs, wz = cz * cs;
    if (obs.Blocked(wx, wz)) return false; // cheapest test first
    const std::optional<f32> g = Ground(wx, wz, refY, /*requireClearance=*/!skipClearance);
    if (!g) return false;
    if (std::fabs(*g - refY) > params_.maxStep) return false; // a wall / cliff between cells
    outGroundY = *g;
    return true;
}

std::vector<glm::vec3> GridNav::FindPath(const glm::vec3& start, const glm::vec3& goal,
                                         const std::vector<GridObstacle>& obstacles) const {
    std::vector<glm::vec3> out;
    if (!Ready()) return out;
    const f32 cs = params_.cellSize;
    const auto cellX = [&](f32 v) { return static_cast<int>(std::lround(v / cs)); };

    // Index the obstacles ONCE for the whole query (see ObstacleGrid).
    ObstacleGrid obs;
    obs.Build(obstacles, params_.agentRadius);

    const int scx = cellX(start.x), scz = cellX(start.z);
    int gcx = cellX(goal.x), gcz = cellX(goal.z);
    const f32 startGround = GroundAt(start.x, start.z, start.y).value_or(start.y);

    // Snap the goal to the nearest walkable cell (so an off-mesh target still routes).
    {
        f32 gy;
        const ObstacleGrid none;
        if (!CellWalkable(gcx, gcz, GroundAt(goal.x, goal.z, goal.y).value_or(goal.y), none, gy)) {
            bool snapped = false;
            for (int r = 1; r <= 16 && !snapped; ++r) {
                for (int dz = -r; dz <= r && !snapped; ++dz)
                    for (int dx = -r; dx <= r && !snapped; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                        if (CellWalkable(gcx + dx, gcz + dz, goal.y, none, gy)) {
                            gcx += dx;
                            gcz += dz;
                            snapped = true;
                        }
                    }
            }
            if (!snapped) return out;
        }
    }

    const auto heur = [&](int cx, int cz) {
        const f32 dx = static_cast<f32>(cx - gcx), dz = static_cast<f32>(cz - gcz);
        return std::sqrt(dx * dx + dz * dz) * cs;
    };

    struct PQ { f32 f, g; int cx, cz; };
    struct Cmp { bool operator()(const PQ& a, const PQ& b) const { return a.f > b.f; } };
    std::priority_queue<PQ, std::vector<PQ>, Cmp> open;
    std::unordered_map<u64, f32> gScore, groundOf;
    std::unordered_map<u64, u64> came;

    const u64 startKey = CellKey(scx, scz);
    gScore[startKey] = 0.0f;
    groundOf[startKey] = startGround;
    open.push({heur(scx, scz), 0.0f, scx, scz});

    u64 bestKey = startKey;
    f32 bestH = heur(scx, scz);
    bool reached = false;
    int expand = 0;
    static const int kDX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int kDZ[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    while (!open.empty() && expand < params_.maxExpand) {
        const PQ cur = open.top();
        open.pop();
        const u64 key = CellKey(cur.cx, cur.cz);
        if (cur.g > gScore[key] + 1e-4f) continue; // stale heap entry
        ++expand;
        if (cur.cx == gcx && cur.cz == gcz) { bestKey = key; reached = true; break; }
        const f32 h = heur(cur.cx, cur.cz);
        if (h < bestH) { bestH = h; bestKey = key; }
        const f32 curGround = groundOf[key];

        for (int d = 0; d < 8; ++d) {
            const int ncx = cur.cx + kDX[d], ncz = cur.cz + kDZ[d];
            const u64 nkeyEarly = CellKey(ncx, ncz);
            f32 ng = 0.0f;
            // MEMOISE the ground. An 8-connected grid samples every cell once per
            // neighbouring expansion - about 8 times - and `groundOf` already holds the
            // resolved ground for every cell A* has improved, so ~7/8 of the sampling
            // was thrown away. Reusing it is safe, not just cheap: the cached value is a
            // real walkable surface at that cell, and accepting it only when it is within
            // maxStep of refY is exactly the test CellWalkable would apply. (Obstacles
            // still have to be tested - they move between queries.)
            bool memo = false;
            if (const auto gi = groundOf.find(nkeyEarly); gi != groundOf.end()) {
                if (std::fabs(gi->second - curGround) <= params_.maxStep &&
                    !obs.Blocked(ncx * cs, ncz * cs)) {
                    ng = gi->second;
                    memo = true;
                }
            }
            // One ring around the agent's own cell is exempt from the clearance test -
            // see the CellWalkable declaration.
            const bool escapeRing =
                std::max(std::abs(ncx - scx), std::abs(ncz - scz)) <= 1;
            if (!memo && !CellWalkable(ncx, ncz, curGround, obs, ng, escapeRing)) continue;
            const f32 stepLen = (kDX[d] != 0 && kDZ[d] != 0) ? 1.41421356f * cs : cs;
            const f32 tentative = cur.g + stepLen + std::fabs(ng - curGround) * 0.5f;
            const u64 nkey = nkeyEarly;
            const auto it = gScore.find(nkey);
            if (it == gScore.end() || tentative < it->second) {
                gScore[nkey] = tentative;
                groundOf[nkey] = ng;
                came[nkey] = key;
                open.push({tentative + heur(ncx, ncz), tentative, ncx, ncz});
            }
        }
    }

    // Reconstruct from the goal (or the closest reached cell = best effort).
    if (!reached && bestKey == startKey) return out; // went nowhere
    std::vector<u64> chain;
    for (u64 k = bestKey;;) {
        chain.push_back(k);
        const auto it = came.find(k);
        if (it == came.end()) break;
        k = it->second;
    }
    std::reverse(chain.begin(), chain.end());
    std::vector<glm::vec3> raw;
    raw.reserve(chain.size());
    for (u64 k : chain) {
        const int cx = static_cast<int>(static_cast<i32>(k >> 32));
        const int cz = static_cast<int>(static_cast<i32>(k & 0xffffffffu));
        raw.push_back({cx * cs, groundOf[k], cz * cs});
    }
    if (raw.empty()) return out;
    raw.front() = {start.x, startGround, start.z}; // exact start

    // String-pull: drop corners the agent can walk past in a straight line.
    const auto segWalkable = [&](const glm::vec3& p0, const glm::vec3& p1) {
        const f32 dist = glm::distance(glm::vec2(p0.x, p0.z), glm::vec2(p1.x, p1.z));
        const int steps = std::max(1, static_cast<int>(dist / (cs * 0.5f)));
        f32 refY = p0.y;
        for (int s = 1; s <= steps; ++s) {
            const f32 tt = static_cast<f32>(s) / steps;
            const f32 x = p0.x + (p1.x - p0.x) * tt;
            const f32 z = p0.z + (p1.z - p0.z) * tt;
            if (obs.Blocked(x, z)) return false;
            const std::optional<f32> g = Ground(x, z, refY, /*requireClearance=*/true);
            if (!g || std::fabs(*g - refY) > params_.maxStep) return false;
            refY = *g;
        }
        return true;
    };

    // BOUNDED string-pull. The obvious form - walk k forward from the anchor and
    // commit k-1 on the first failure - re-tests the whole span from the anchor for
    // every k, so it costs sum(2k) ground samples: ~48,000 for a 220-cell path, about
    // 90% of the cost of a query (A* itself is ~5%). It is quadratic in path length,
    // and a 512 m walkable terrain makes long paths the normal case. Instead: probe
    // exponentially for the furthest visible cell, then bisect. Same corners, O(n log n)
    // samples. (Visibility is assumed monotone along the raw path - the same assumption
    // the greedy scan already made.)
    out.push_back(raw.front());
    size_t anchor = 0;
    while (anchor + 1 < raw.size()) {
        const size_t last = raw.size() - 1;
        size_t vis = anchor + 1; // the adjacent raw cell is always reachable
        size_t reach = 1;
        while (vis < last) {
            const size_t probe = std::min(last, anchor + reach * 2);
            if (probe <= vis || !segWalkable(raw[anchor], raw[probe])) break;
            vis = probe;
            reach *= 2;
        }
        // Bisect between the last visible cell and the first blocked one.
        size_t hi = std::min(last, anchor + reach * 2);
        while (vis + 1 < hi) {
            const size_t mid = vis + (hi - vis) / 2;
            if (segWalkable(raw[anchor], raw[mid]))
                vis = mid;
            else
                hi = mid;
        }
        anchor = vis;
        out.push_back(raw[anchor]);
    }

    // --- Human-path smoothing -------------------------------------------------
    // String-pulling gives the shortest path but with SHARP corners that hug walls
    // and read as robotic. Round each corner into a natural curve with validated
    // Chaikin corner-cutting: a corner is only rounded when BOTH new points and the
    // chord between them stay on walkable ground clear of obstacles, so a rounded
    // turn never clips a wall - tight spots (doorways) keep their sharp corner. Two
    // passes give a smooth arc; endpoints (exact start / goal) stay pinned.
    const auto snapWalkable = [&](glm::vec3& p, f32 refY) -> bool {
        if (obs.Blocked(p.x, p.z)) return false;
        const std::optional<f32> g = Ground(p.x, p.z, refY, /*requireClearance=*/true);
        if (!g || std::fabs(*g - refY) > params_.maxStep) return false;
        p.y = *g; // ride the ground
        return true;
    };
    for (int iter = 0; iter < 2 && out.size() >= 3; ++iter) {
        std::vector<glm::vec3> sm;
        sm.reserve(out.size() * 2);
        sm.push_back(out.front());
        for (size_t i = 1; i + 1 < out.size(); ++i) {
            glm::vec3 q = out[i] + (out[i - 1] - out[i]) * 0.25f; // 1/4 toward prev
            glm::vec3 r = out[i] + (out[i + 1] - out[i]) * 0.25f; // 1/4 toward next
            if (snapWalkable(q, out[i].y) && snapWalkable(r, out[i].y) && segWalkable(q, r)) {
                sm.push_back(q);
                sm.push_back(r);
            } else {
                sm.push_back(out[i]); // can't round here (tight) - keep the corner
            }
        }
        sm.push_back(out.back());
        out.swap(sm);
    }
    return out;
}

void GridNav::DebugCells(const glm::vec3& center, f32 extent, std::vector<glm::vec3>& out) const {
    out.clear();
    if (!Ready()) return;
    const f32 cs = params_.cellSize;
    const int n = std::min(80, static_cast<int>(extent / cs));
    const int ccx = static_cast<int>(std::lround(center.x / cs));
    const int ccz = static_cast<int>(std::lround(center.z / cs));
    for (int dz = -n; dz <= n; ++dz)
        for (int dx = -n; dx <= n; ++dx) {
            const f32 x = (ccx + dx) * cs, z = (ccz + dz) * cs;
            if (const std::optional<f32> g = GroundAt(x, z, center.y))
                out.push_back({x, *g, z});
        }
}

// --- Agent steering (GridNav overload of nav::UpdateAgents) ------------------
void UpdateAgents(Scene& scene, const GridNav& grid, f32 dt) {
    if (!grid.Ready() || dt <= 0.0f) return;
    entt::registry& reg = scene.Registry();

    std::vector<GridObstacle> obstacles;
    for (const entt::entity e : reg.view<Transform, NavigationObstacle>()) {
        const NavigationObstacle& o = reg.get<NavigationObstacle>(e);
        if (o.enabled) obstacles.push_back({glm::vec3(scene.WorldMatrix(e)[3]), o.radius});
    }

    const auto horiz = [](const glm::vec3& v) { return glm::vec2(v.x, v.z); };

    // --- Per-frame A* budget --------------------------------------------------
    // UpdateAgents runs exactly once per frame, so a LOCAL counter IS the frame budget.
    // Without it: `ag.path.empty()` is tested first and unconditionally, and an
    // unreachable target (walled off, or past the maxExpand range ceiling) leaves the
    // path empty - so a single stuck agent re-ran a full budget-exhausting query EVERY
    // FRAME, FOREVER, measured at ~4.7 ms against ~0.4 ms of total CPU headroom. The
    // jittered cooldown was never consulted on that branch. `targetMoved` bypassed it
    // too, so a squad handed a target in one frame issued N queries in that frame.
    int queryBudget = std::max(1, grid.Params().maxQueriesPerFrame);
    grid.ResetQueryCount();

    // --- Agent avoidance index ------------------------------------------------
    // The separation loop used to iterate every other agent inside the per-agent loop
    // with two sparse-set lookups per pair: O(agents^2), ~0.8 ms at 200 agents, twice
    // the whole frame's headroom. Gather once into a flat array and bucket it.
    struct AgentP { glm::vec2 xz; f32 radius; entt::entity e; };
    std::vector<AgentP> nearby;
    for (const entt::entity e : reg.view<Transform, NavigationAgent>())
        nearby.push_back({horiz(reg.get<Transform>(e).position),
                          reg.get<NavigationAgent>(e).radius, e});
    // Uniform hash grid over the agents. Cell = the largest separation distance any
    // pair can care about, so one 3x3 neighbourhood covers every possible interaction.
    f32 agentCell = 1.0f;
    for (const AgentP& a : nearby) agentCell = std::max(agentCell, a.radius * 2.0f);
    std::unordered_map<u64, std::vector<int>> agentCells;
    const auto agentCellKey = [&](glm::vec2 p) {
        return CellKey(static_cast<int>(std::floor(p.x / agentCell)),
                       static_cast<int>(std::floor(p.y / agentCell)));
    };
    if (nearby.size() > 8) { // below that the flat scan is already cheaper than hashing
        for (int i = 0; i < static_cast<int>(nearby.size()); ++i)
            agentCells[agentCellKey(nearby[static_cast<size_t>(i)].xz)].push_back(i);
    }

    for (const entt::entity e : reg.view<Transform, NavigationAgent>()) {
        NavigationAgent& ag = reg.get<NavigationAgent>(e);
        Transform& tf = reg.get<Transform>(e);
        const glm::vec3 pos = tf.position;

        if (!ag.hasTarget) {
            ag.velocity *= std::max(0.0f, 1.0f - dt * 6.0f);
            continue;
        }

        // Re-plan on first run, target change, exhausted path, or the periodic
        // cooldown - the cooldown is what makes it reroute around MOVING obstacles.
        ag.repathCooldown -= dt;
        const bool targetMoved = glm::distance(ag.target, ag.lastTarget) > 0.25f;
        const bool wants = ag.path.empty() || (ag.autoRepath && targetMoved) ||
                           ag.corner >= ag.path.size() || ag.repathCooldown <= 0.0f;
        // The cooldown now gates EVERY re-plan reason, including the empty-path one, so a
        // stuck agent retries at ~3 Hz instead of 120 Hz; and the frame budget caps how
        // many agents can pay for a query in the same frame. An agent that wants a query
        // but does not get one keeps whatever path it has and asks again shortly - it
        // never blocks, and the cost never stacks.
        if (wants && ag.repathCooldown <= 0.0f && queryBudget > 0) {
            --queryBudget;
            grid.NoteQuery();
            ag.path = grid.FindPath(pos, ag.target, obstacles);
            ag.lastTarget = ag.target;
            ag.corner = ag.path.size() > 1 ? 1u : 0u;
            ag.reached = false;
            // ~3 Hz, JITTERED per entity so a squad that acquires a target in the same
            // frame does not stack every re-plan into the same frame forever after.
            const u32 j = static_cast<u32>(Mix64(static_cast<u64>(entt::to_integral(e))) & 0xffu);
            ag.repathCooldown = 0.27f + 0.06f * (static_cast<f32>(j) / 255.0f);
        } else if (wants && ag.repathCooldown <= 0.0f) {
            // Budget exhausted this frame. Retry next frame rather than next cooldown.
            ag.repathCooldown = 0.0f;
        }
        if (ag.path.empty()) {
            ag.velocity = glm::vec3(0.0f);
            continue;
        }

        if (glm::distance(horiz(pos), horiz(ag.path.back())) <= ag.stoppingDistance) {
            ag.reached = true;
            ag.velocity *= std::max(0.0f, 1.0f - dt * 8.0f);
            continue;
        }
        ag.reached = false;

        if (ag.corner < ag.path.size()) {
            if (glm::distance(horiz(pos), horiz(ag.path[ag.corner])) < std::max(ag.radius, 0.3f) &&
                ag.corner + 1 < ag.path.size())
                ++ag.corner;
        }
        const glm::vec3 corner = ag.path[std::min<u32>(ag.corner, (u32)ag.path.size() - 1)];

        glm::vec3 desired(corner.x - pos.x, 0.0f, corner.z - pos.z);
        const f32 toCorner = glm::length(desired);
        desired = toCorner > 1e-4f ? desired / toCorner * ag.speed : glm::vec3(0.0f);

        // Soft local avoidance between re-plans (obstacles + other agents).
        for (const GridObstacle& o : obstacles) {
            glm::vec3 away(pos.x - o.pos.x, 0.0f, pos.z - o.pos.z);
            const f32 d = glm::length(away);
            const f32 influence = o.radius + ag.radius;
            if (d < influence && d > 1e-4f)
                desired += away / d * ag.speed * (1.0f - d / influence) * 1.5f;
        }
        const auto separate = [&](const AgentP& o) {
            if (o.e == e) return;
            glm::vec2 away = horiz(pos) - o.xz;
            const f32 d = glm::length(away);
            const f32 sep = ag.radius + o.radius;
            if (d < sep && d > 1e-4f) {
                const glm::vec2 push = away / d * ag.speed * (1.0f - d / sep);
                desired.x += push.x;
                desired.z += push.y;
            }
        };
        if (agentCells.empty()) {
            for (const AgentP& o : nearby) separate(o);
        } else {
            const int bx = static_cast<int>(std::floor(pos.x / agentCell));
            const int bz = static_cast<int>(std::floor(pos.z / agentCell));
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto ci = agentCells.find(CellKey(bx + dx, bz + dz));
                    if (ci == agentCells.end()) continue;
                    for (const int i : ci->second) separate(nearby[static_cast<size_t>(i)]);
                }
        }
        if (const f32 dl = glm::length(desired); dl > ag.speed) desired = desired / dl * ag.speed;

        glm::vec3 dv = desired - ag.velocity;
        if (const f32 m = glm::length(dv); m > ag.acceleration * dt)
            dv = dv / m * (ag.acceleration * dt);
        ag.velocity += dv;
        ag.velocity.y = 0.0f;

        glm::vec3 newPos = pos + ag.velocity * dt;
        if (const std::optional<f32> g = grid.GroundAt(newPos.x, newPos.z, pos.y))
            newPos.y = *g; // follow the ground
        tf.position = newPos;

        if (ag.turnSpeed > 0.0f && glm::length(horiz(ag.velocity)) > 0.05f) {
            const f32 yaw = std::atan2(ag.velocity.x, ag.velocity.z);
            const glm::quat want = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            tf.rotation =
                glm::normalize(glm::slerp(tf.rotation, want, std::min(1.0f, ag.turnSpeed * dt)));
        }
    }
}

} // namespace hbe::nav
