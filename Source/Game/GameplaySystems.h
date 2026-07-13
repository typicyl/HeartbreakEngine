// Game/GameplaySystems.h - the single gameplay update band.
//
// gameplay::Update is the one call site the engine ticks (while the simulation
// runs) that sequences the gameplay sub-systems in a fixed, documented order:
//   AI (sense + decide + set nav targets + fire)  ->  spawning / encounters  ->
//   combat (regen, i-frames, weapon cooldowns, death dispatch)  ->  player fire.
// It runs AFTER physics (fresh positions for line-of-sight + hits) and BEFORE
// nav::UpdateAgents, so AI-set NavigationAgent targets steer the same frame.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;
class PhysicsWorld;
class Renderer;
class Input;
class Camera;

namespace gameplay {

void Update(Scene& scene, PhysicsWorld& physics, Renderer& renderer, const Input& input,
            const Camera& camera, f32 dt);

} // namespace gameplay
} // namespace hbe
