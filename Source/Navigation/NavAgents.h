// Navigation/NavAgents.h - the engine's agent driver over NavWorld.
//
// These free functions are the ONLY navigation entry points gameplay touches at the
// system level; per-entity, gameplay sets NavigationAgent.target / hasTarget and reads
// NavigationAgent.velocity / reached (unchanged from the old grid pathfinder), so AI,
// combat, animation and the character controller keep working without knowing Detour
// exists.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;

namespace nav {

class NavWorld;

// Mirror every NavigationObstacle entity into the navmesh as a dynamic (dtTileCache)
// obstacle, and drop obstacles whose entity vanished. MUST run before NavWorld::Update
// (so the tile-cache rebuild this frame sees the current obstacle set).
void SyncObstacles(Scene& scene, NavWorld& world);

// Steer every NavigationAgent toward its target along a Detour path, re-planning
// periodically (and on target change) so it reroutes around dynamic obstacles, snaps
// each agent onto the navmesh, faces movement, and softly avoids obstacles + other
// agents. When required navigation tiles are not resident yet it requests them and
// keeps the current path instead of stopping. Once per frame while simulating.
void UpdateAgents(Scene& scene, NavWorld& world, f32 dt);

} // namespace nav
} // namespace hbe
