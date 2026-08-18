// Vegetation/VegetationSurface.h - the terrain -> vegetation query bridge.
//
// Realizes the owner's VegetationSpawnContext by reusing the EXISTING analytic terrain
// queries (terrain::SampleSurface - the main-diagonal triangulation that matches the
// collider and shader), the terrain splat mask as a biome/material proxy, and the water
// surface height. It returns a plain SurfaceQueryFn callback so a distribution filters
// on SpawnSample without ever depending on Scene / TerrainComponent - keeping generation
// job-safe and unit-testable. No NEW terrain query system (design doc section 6).
#pragma once

#include "Vegetation/VegetationTypes.h"

#include <entt/entt.hpp>

namespace hbe {

class Scene;

namespace veg {

// Finds the (first) terrain entity in the scene. Returns entt::null if there is none.
// There is no engine-wide "get the terrain" helper, so this is it.
entt::entity FindTerrain(const Scene& scene);

// Builds a surface-query callback over the scene's terrain (+ water + weather). Returns
// an EMPTY function when the scene has no terrain (callers then treat every point as a
// flat, on-ground plane). The returned callback captures the terrain component and its
// world transform by value/pointer; it is valid only while the terrain is NOT being
// mutated (sculpting), which the streamer/editor fences against (design doc section 6).
SurfaceQueryFn MakeTerrainSurfaceQuery(Scene& scene);

} // namespace veg
} // namespace hbe
