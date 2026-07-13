// Game/AISystem.h - enemy/NPC perception + behavior FSM.
//
// ai::Update senses the world (sight cone + line-of-sight raycast, hearing), runs
// each AIBehavior state machine, sets the entity's NavigationAgent target, and
// fires its Weapon at hostiles via combat::TryFire. Ticked by gameplay::Update
// BEFORE nav::UpdateAgents so a target set this frame steers the same frame.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;
class PhysicsWorld;

namespace ai {

void Update(Scene& scene, PhysicsWorld& physics, f32 dt);

} // namespace ai
} // namespace hbe
