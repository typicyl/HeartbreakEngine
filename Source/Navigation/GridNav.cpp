// Navigation/GridNav.cpp - real-time grid A* + ground query implementation.
#include "Navigation/GridNav.h"

#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

namespace hbe::nav {

namespace {
u64 CellKey(int cx, int cz) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
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

// Gathers world-space static triangles from the scene's meshes (rebuilding CPU
// geometry from each entity's MeshRef provenance). Filter precedence: Static
// layer -> NavmeshInput tags -> every mesh.
bool GatherStaticGeometry(const Scene& scene, const std::filesystem::path& assetsDir,
                          std::vector<f32>& verts, std::vector<i32>& tris) {
    const entt::registry& reg = scene.Registry();
    verts.clear();
    tris.clear();

    const auto addSubmesh = [&](const MeshData& md, const glm::mat4& world) {
        const int base = static_cast<int>(verts.size() / 3);
        for (const Vertex& v : md.vertices) {
            const glm::vec3 wp = glm::vec3(world * glm::vec4(v.position, 1.0f));
            verts.push_back(wp.x);
            verts.push_back(wp.y);
            verts.push_back(wp.z);
        }
        for (size_t i = 0; i + 2 < md.indices.size(); i += 3) {
            tris.push_back(base + static_cast<int>(md.indices[i]));
            tris.push_back(base + static_cast<int>(md.indices[i + 1]));
            tris.push_back(base + static_cast<int>(md.indices[i + 2]));
        }
    };

    enum class Filter { Static, Input, All } filter = Filter::All;
    for (const entt::entity e : reg.view<const SceneLayer>()) {
        if (reg.get<const SceneLayer>(e).kind == SceneKind::Static) {
            filter = Filter::Static;
            break;
        }
    }
    if (filter == Filter::All) {
        auto inputs = reg.view<const NavmeshInput>();
        if (inputs.begin() != inputs.end()) filter = Filter::Input;
    }

    for (const entt::entity e : reg.view<const Transform, const MeshRef>()) {
        if (filter == Filter::Static) {
            const SceneLayer* sl = reg.try_get<SceneLayer>(e);
            if (!sl || sl->kind != SceneKind::Static) continue;
            const NavmeshInput* ni = reg.try_get<NavmeshInput>(e);
            if (ni && !ni->enabled) continue; // explicit opt-out of a static mesh
        } else if (filter == Filter::Input) {
            const NavmeshInput* ni = reg.try_get<NavmeshInput>(e);
            if (!ni || !ni->enabled) continue;
        }
        const std::string& src = reg.get<const MeshRef>(e).source;
        const glm::mat4 world = scene.WorldMatrix(e);
        if (src.rfind("prim:", 0) == 0) {
            MeshData md = mesh::GeneratePrimitive(src.substr(5));
            if (!md.vertices.empty()) addSubmesh(md, world);
        } else if (src.rfind("uaf:", 0) == 0) {
            std::string rel = src.substr(4);
            int submesh = 0;
            if (const auto h = rel.find_last_of('#'); h != std::string::npos) {
                submesh = std::atoi(rel.c_str() + h + 1);
                rel = rel.substr(0, h);
            }
            if (std::optional<Model> model = uaf::ReadMesh(assetsDir / rel);
                model && submesh >= 0 && submesh < static_cast<int>(model->size())) {
                addSubmesh((*model)[static_cast<size_t>(submesh)], world);
            }
        }
    }
    return !verts.empty() && !tris.empty();
}
} // namespace

void GridNav::Clear() {
    tris_.clear();
    buckets_.clear();
    gw_ = gh_ = 0;
    signature_ = 0;
}

const std::vector<int>& GridNav::BucketAt(f32 x, f32 z) const {
    static const std::vector<int> empty;
    if (gw_ <= 0 || gh_ <= 0) return empty;
    const int ix = static_cast<int>(std::floor((x - gridMin_.x) / bucket_));
    const int iz = static_cast<int>(std::floor((z - gridMin_.y) / bucket_));
    if (ix < 0 || iz < 0 || ix >= gw_ || iz >= gh_) return empty;
    return buckets_[static_cast<size_t>(iz) * gw_ + ix];
}

void GridNav::Rebuild(const Scene& scene, const std::filesystem::path& assetsDir) {
    Clear();
    std::vector<f32> verts;
    std::vector<i32> idx;
    if (!GatherStaticGeometry(scene, assetsDir, verts, idx)) return;

    const f32 cosSlope = std::cos(glm::radians(params_.maxSlopeDeg));
    glm::vec2 mn(1e30f), mx(-1e30f);
    tris_.reserve(idx.size() / 3);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        Tri t;
        const auto V = [&](i32 k) {
            return glm::vec3(verts[k * 3], verts[k * 3 + 1], verts[k * 3 + 2]);
        };
        t.a = V(idx[i]);
        t.b = V(idx[i + 1]);
        t.c = V(idx[i + 2]);
        const glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        const f32 len = glm::length(n);
        t.walkable = len > 1e-6f && (std::fabs(n.y) / len) >= cosSlope;
        tris_.push_back(t);
        for (const glm::vec3& p : {t.a, t.b, t.c}) {
            mn.x = std::min(mn.x, p.x);
            mn.y = std::min(mn.y, p.z);
            mx.x = std::max(mx.x, p.x);
            mx.y = std::max(mx.y, p.z);
        }
    }
    if (tris_.empty()) return;

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
        const Tri& t = tris_[ti];
        const f32 x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
        const f32 z0 = std::min({t.a.z, t.b.z, t.c.z}), z1 = std::max({t.a.z, t.b.z, t.c.z});
        const int ix0 = std::max(0, static_cast<int>(std::floor((x0 - gridMin_.x) / bucket_)));
        const int ix1 = std::min(gw_ - 1, static_cast<int>(std::floor((x1 - gridMin_.x) / bucket_)));
        const int iz0 = std::max(0, static_cast<int>(std::floor((z0 - gridMin_.y) / bucket_)));
        const int iz1 = std::min(gh_ - 1, static_cast<int>(std::floor((z1 - gridMin_.y) / bucket_)));
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix)
                buckets_[static_cast<size_t>(iz) * gw_ + ix].push_back(ti);
    }
    HBE_INFO("GridNav: built height index from {} static triangles ({}x{} buckets).",
             static_cast<u32>(tris_.size()), gw_, gh_);
}

bool GridNav::EnsureBuilt(const Scene& scene, const std::filesystem::path& assetsDir) {
    // Fingerprint the static-geometry set so we rebuild on level load / streaming.
    const entt::registry& reg = scene.Registry();
    u64 sig = 1469598103934665603ull; // FNV offset
    const auto mix = [&](u64 v) { sig = (sig ^ v) * 1099511628211ull; };
    u32 count = 0;
    for (const entt::entity e : reg.view<const SceneLayer, const MeshRef>()) {
        if (reg.get<const SceneLayer>(e).kind != SceneKind::Static) continue;
        ++count;
        mix(static_cast<u64>(entt::to_integral(e)));
    }
    // No SceneLayer tags anywhere -> fingerprint all MeshRef entities (legacy scenes).
    if (count == 0) {
        for (const entt::entity e : reg.view<const MeshRef>()) {
            ++count;
            mix(static_cast<u64>(entt::to_integral(e)));
        }
    }
    mix(count);
    if (sig == signature_ && Ready()) return true;
    Rebuild(scene, assetsDir);
    signature_ = sig;
    return Ready();
}

std::optional<f32> GridNav::GroundAt(f32 x, f32 z, f32 nearY) const {
    const std::vector<int>& b = BucketAt(x, z);
    f32 best = 0.0f;
    f32 bestDist = params_.climb;
    bool found = false;
    for (int ti : b) {
        const Tri& t = tris_[ti];
        if (!t.walkable) continue;
        f32 y;
        if (!TriHeight(t.a, t.b, t.c, x, z, y)) continue;
        const f32 d = std::fabs(y - nearY);
        if (d <= params_.climb && (!found || d < bestDist)) { // closest surface to nearY
            best = y;
            bestDist = d;
            found = true;
        }
    }
    return found ? std::optional<f32>(best) : std::nullopt;
}

bool GridNav::CellWalkable(int cx, int cz, f32 refY, const std::vector<GridObstacle>& obs,
                           f32& outGroundY) const {
    const f32 cs = params_.cellSize;
    const f32 wx = cx * cs, wz = cz * cs;
    const std::optional<f32> g = GroundAt(wx, wz, refY);
    if (!g) return false;
    if (std::fabs(*g - refY) > params_.maxStep) return false; // a wall / cliff between cells
    for (const GridObstacle& o : obs) {
        const f32 dx = wx - o.pos.x, dz = wz - o.pos.z;
        const f32 r = o.radius + params_.agentRadius;
        if (dx * dx + dz * dz < r * r) return false; // blocked by a (dynamic) obstacle
    }
    outGroundY = *g;
    return true;
}

std::vector<glm::vec3> GridNav::FindPath(const glm::vec3& start, const glm::vec3& goal,
                                         const std::vector<GridObstacle>& obstacles) const {
    std::vector<glm::vec3> out;
    if (!Ready()) return out;
    const f32 cs = params_.cellSize;
    const auto cellX = [&](f32 v) { return static_cast<int>(std::lround(v / cs)); };

    const int scx = cellX(start.x), scz = cellX(start.z);
    int gcx = cellX(goal.x), gcz = cellX(goal.z);
    const f32 startGround = GroundAt(start.x, start.z, start.y).value_or(start.y);

    // Snap the goal to the nearest walkable cell (so an off-mesh target still routes).
    {
        f32 gy;
        std::vector<GridObstacle> none;
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
            f32 ng;
            if (!CellWalkable(ncx, ncz, curGround, obstacles, ng)) continue;
            const f32 stepLen = (kDX[d] != 0 && kDZ[d] != 0) ? 1.41421356f * cs : cs;
            const f32 tentative = cur.g + stepLen + std::fabs(ng - curGround) * 0.5f;
            const u64 nkey = CellKey(ncx, ncz);
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
    for (u64 k = bestKey;; ) {
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
            const std::optional<f32> g = GroundAt(x, z, refY);
            if (!g || std::fabs(*g - refY) > params_.maxStep) return false;
            for (const GridObstacle& o : obstacles) {
                const f32 ddx = x - o.pos.x, ddz = z - o.pos.z;
                const f32 r = o.radius + params_.agentRadius;
                if (ddx * ddx + ddz * ddz < r * r) return false;
            }
            refY = *g;
        }
        return true;
    };
    out.push_back(raw.front());
    size_t anchor = 0;
    for (size_t k = 2; k < raw.size(); ++k) {
        if (!segWalkable(raw[anchor], raw[k])) {
            out.push_back(raw[k - 1]);
            anchor = k - 1;
        }
    }
    out.push_back(raw.back());
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
        if (ag.path.empty() || (ag.autoRepath && targetMoved) || ag.corner >= ag.path.size() ||
            ag.repathCooldown <= 0.0f) {
            ag.path = grid.FindPath(pos, ag.target, obstacles);
            ag.lastTarget = ag.target;
            ag.corner = ag.path.size() > 1 ? 1u : 0u;
            ag.reached = false;
            ag.repathCooldown = 0.3f; // ~3 Hz replan
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
        for (const entt::entity other : reg.view<Transform, NavigationAgent>()) {
            if (other == e) continue;
            glm::vec3 away = pos - reg.get<Transform>(other).position;
            away.y = 0.0f;
            const f32 d = glm::length(away);
            const f32 sep = ag.radius + reg.get<NavigationAgent>(other).radius;
            if (d < sep && d > 1e-4f) desired += away / d * ag.speed * (1.0f - d / sep);
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
