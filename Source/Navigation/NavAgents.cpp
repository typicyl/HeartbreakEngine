// Navigation/NavAgents.cpp - see NavAgents.h. Steering ported from the previous grid
// pathfinder; the only change is the path SOURCE (NavWorld/Detour) and the ground
// snap (navmesh sample), so NavigationAgent behaviour is preserved bit-for-bit at the
// component level.
#include "Navigation/NavAgents.h"
#include "Navigation/NavWorld.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace hbe {
namespace nav {
namespace {

// At most this many path queries per frame across ALL agents. Detour tiled queries are
// far cheaper than the old grid A*, so the cap is higher, but it still turns N stacked
// queries into a bounded stream; agents that miss out keep their path and retry.
constexpr int kMaxQueriesPerFrame = 8;

inline u64 Mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}
inline u64 CellKey(int x, int z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u64>(static_cast<u32>(z));
}
inline glm::vec2 Horiz(const glm::vec3& v) { return glm::vec2(v.x, v.z); }

struct SoftObstacle {
    glm::vec3 pos{0.0f};
    f32 radius = 1.0f;
};

} // namespace

void SyncObstacles(Scene& scene, NavWorld& world) {
    if (!world.Loaded()) return;
    entt::registry& reg = scene.Registry();
    std::vector<NavWorld::ObstacleDesc> descs;
    for (const entt::entity e : reg.view<Transform, NavigationObstacle>()) {
        const NavigationObstacle& o = reg.get<NavigationObstacle>(e);
        if (!o.enabled) continue;
        NavWorld::ObstacleDesc d;
        d.id = static_cast<u64>(entt::to_integral(e));
        d.pos = glm::vec3(scene.WorldMatrix(e)[3]);
        d.radius = o.radius;
        d.height = o.height;
        descs.push_back(d);
    }
    world.SyncObstacles(descs);
}

void UpdateAgents(Scene& scene, NavWorld& world, f32 dt) {
    if (!world.Loaded() || dt <= 0.0f) return;
    entt::registry& reg = scene.Registry();

    // Soft local-avoidance obstacles (the same NavigationObstacles feed the hard nav
    // rebuild in SyncObstacles; this is the between-replan smoothing on top).
    std::vector<SoftObstacle> obstacles;
    for (const entt::entity e : reg.view<Transform, NavigationObstacle>()) {
        const NavigationObstacle& o = reg.get<NavigationObstacle>(e);
        if (o.enabled) obstacles.push_back({glm::vec3(scene.WorldMatrix(e)[3]), o.radius});
    }

    // Agent avoidance index (uniform hash grid; one 3x3 neighbourhood covers any pair).
    struct AgentP {
        glm::vec2 xz;
        f32 radius;
        entt::entity e;
    };
    std::vector<AgentP> nearby;
    for (const entt::entity e : reg.view<Transform, NavigationAgent>())
        nearby.push_back({Horiz(glm::vec3(scene.WorldMatrix(e)[3])),
                          reg.get<NavigationAgent>(e).radius, e});
    f32 agentCell = 1.0f;
    for (const AgentP& a : nearby) agentCell = std::max(agentCell, a.radius * 2.0f);
    std::unordered_map<u64, std::vector<int>> agentCells;
    const auto agentCellKey = [&](glm::vec2 p) {
        return CellKey(static_cast<int>(std::floor(p.x / agentCell)),
                       static_cast<int>(std::floor(p.y / agentCell)));
    };
    if (nearby.size() > 8) {
        for (int i = 0; i < static_cast<int>(nearby.size()); ++i)
            agentCells[agentCellKey(nearby[static_cast<size_t>(i)].xz)].push_back(i);
    }

    int queryBudget = kMaxQueriesPerFrame;

    for (const entt::entity e : reg.view<Transform, NavigationAgent>()) {
        NavigationAgent& ag = reg.get<NavigationAgent>(e);
        Transform& tf = reg.get<Transform>(e);
        const glm::vec3 pos = glm::vec3(scene.WorldMatrix(e)[3]); // WORLD (parent-aware)

        if (!ag.hasTarget) {
            ag.velocity *= std::max(0.0f, 1.0f - dt * 6.0f);
            continue;
        }

        ag.repathCooldown -= dt;
        const bool targetMoved = glm::distance(ag.target, ag.lastTarget) > 0.25f;
        const bool wants = ag.path.empty() || (ag.autoRepath && targetMoved) ||
                           ag.corner >= ag.path.size() || ag.repathCooldown <= 0.0f;
        if (wants && ag.repathCooldown <= 0.0f && queryBudget > 0) {
            --queryBudget;
            bool missing = false;
            std::vector<glm::vec3> corners;
            const bool ok = world.FindPath(pos, ag.target, corners, &missing);
            if (ok) {
                ag.path = std::move(corners);
                ag.lastTarget = ag.target;
                ag.corner = ag.path.size() > 1 ? 1u : 0u;
                ag.reached = false;
                const u32 j = static_cast<u32>(Mix64(static_cast<u64>(entt::to_integral(e))) & 0xffu);
                ag.repathCooldown = 0.27f + 0.06f * (static_cast<f32>(j) / 255.0f);
            } else if (missing) {
                // Required tiles are still streaming: keep the current path, remember the
                // target so we do not thrash targetMoved, and retry soon (RequestAround
                // was already issued inside FindPath).
                ag.lastTarget = ag.target;
                ag.repathCooldown = 0.1f;
            } else {
                // Genuinely unreachable on the resident navmesh: stop.
                ag.path.clear();
                ag.lastTarget = ag.target;
                ag.repathCooldown = 0.27f;
            }
        } else if (wants && ag.repathCooldown <= 0.0f) {
            ag.repathCooldown = 0.0f; // budget exhausted; retry next frame
        }
        if (ag.path.empty()) {
            ag.velocity = glm::vec3(0.0f);
            continue;
        }

        if (glm::distance(Horiz(pos), Horiz(ag.path.back())) <= ag.stoppingDistance) {
            ag.reached = true;
            ag.velocity *= std::max(0.0f, 1.0f - dt * 8.0f);
            continue;
        }
        ag.reached = false;

        if (ag.corner < ag.path.size()) {
            if (glm::distance(Horiz(pos), Horiz(ag.path[ag.corner])) < std::max(ag.radius, 0.3f) &&
                ag.corner + 1 < ag.path.size())
                ++ag.corner;
        }
        const glm::vec3 corner = ag.path[std::min<u32>(ag.corner, static_cast<u32>(ag.path.size()) - 1)];

        glm::vec3 desired(corner.x - pos.x, 0.0f, corner.z - pos.z);
        const f32 toCorner = glm::length(desired);
        desired = toCorner > 1e-4f ? desired / toCorner * ag.speed : glm::vec3(0.0f);

        for (const SoftObstacle& o : obstacles) {
            glm::vec3 away(pos.x - o.pos.x, 0.0f, pos.z - o.pos.z);
            const f32 d = glm::length(away);
            const f32 influence = o.radius + ag.radius;
            if (d < influence && d > 1e-4f)
                desired += away / d * ag.speed * (1.0f - d / influence) * 1.5f;
        }
        const auto separate = [&](const AgentP& o) {
            if (o.e == e) return;
            glm::vec2 away = Horiz(pos) - o.xz;
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
        // Follow the navmesh surface: snap Y to the nearest walkable point. If the tile
        // under the new position is not resident, keep the integrated height.
        glm::vec3 snapped;
        if (world.SamplePoint(newPos, snapped)) newPos.y = snapped.y;
        scene.SetWorldPosition(e, newPos);

        if (ag.turnSpeed > 0.0f && glm::length(Horiz(ag.velocity)) > 0.05f) {
            const f32 yaw = std::atan2(-ag.velocity.x, -ag.velocity.z); // forward = -Z
            const glm::quat want = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 wm = scene.WorldMatrix(e);
            glm::vec3 b0(wm[0]), b1(wm[1]), b2(wm[2]);
            const f32 n0 = glm::length(b0), n1 = glm::length(b1), n2 = glm::length(b2);
            const glm::quat curWorld = (n0 > 1e-8f && n1 > 1e-8f && n2 > 1e-8f)
                                           ? glm::quat_cast(glm::mat3(b0 / n0, b1 / n1, b2 / n2))
                                           : tf.rotation;
            scene.SetWorldRotation(e, glm::normalize(glm::slerp(glm::normalize(curWorld), want,
                                                                std::min(1.0f, ag.turnSpeed * dt))));
        }
    }
}

} // namespace nav
} // namespace hbe
