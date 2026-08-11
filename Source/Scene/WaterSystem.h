// Scene/WaterSystem.h - Gerstner-wave water surface driver.
//
// Builds/regenerates each WaterComponent's flat grid mesh (attaching a Water|Transparent|
// NoShadow MeshInstance that draws in the dedicated water pass; the Gerstner displacement
// lives in the water VS), ages the scene-global interactive ripple ring buffer, and spawns
// rain-splash rings while it is raining. Wave/colour/ripple params are global for the scene
// (the FIRST water entity drives them); MakeView reads them into the SceneView.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe {

class Scene;
class Renderer;
class PhysicsWorld;

namespace water {

// World-space water surface height at (x, z) from the FIRST WaterComponent + the shared scene
// clock (matches the Gerstner shader). Returns a very low value when there is no water. Useful
// for gameplay ("is this point underwater?") as well as buoyancy.
f32 SurfaceHeightAt(Scene& scene, f32 x, f32 z);

// Apply buoyancy so dynamic rigid bodies float + bob on the water. Call each frame in play
// mode, BEFORE the physics step (queues forces the step then integrates).
void ApplyBuoyancy(Scene& scene, PhysicsWorld& physics, f32 dt);

// Call once per frame before RenderScene (needs the renderer for mesh upload + camera).
void Update(Scene& scene, Renderer& renderer, f32 dt);

// Spawn an interactive ripple ring at a world position (object / player splash). Safe from
// gameplay; drops into the scene-global ring buffer (oldest evicted when full).
void AddRipple(Scene& scene, const glm::vec3& worldPos, f32 strength = 0.7f);

} // namespace water
} // namespace hbe
