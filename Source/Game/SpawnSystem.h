// Game/SpawnSystem.h - runtime spawning + encounter management.
//
// spawn::Update ticks Spawner components: on their trigger (player-in-volume /
// story flag / manual/schematic request) it instantiates a `.hbprefab` `count`
// times via scene::Instantiate, tags each spawned root with Spawned{encounter,
// spawner}, throttles to maxAlive, and handles respawn + despawn. Encounters group
// spawners by id, tally alive members by scanning Spawned tags (stable across the
// .hbsave Replace, unlike raw entt handles), and fire a completion InteractAction
// when cleared. spawn::Update runs before combat reads the spawned entities.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;
class Renderer;

namespace spawn {

// Ticks Spawners: trigger eval, prefab burst instantiation, tagging, throttle,
// respawn, despawn. Needs the Renderer to upload spawned GPU resources.
void Update(Scene& scene, Renderer& renderer, f32 dt);

// Recomputes each Encounter's alive count from Spawned tags, detects the cleared
// edge, and fires the completion action. Runs after Update() each frame.
void UpdateEncounters(Scene& scene, f32 dt);

// Drops the cached parsed/staged prefabs (project switch / prefab re-saved).
void ClearPrefabCache();

} // namespace spawn
} // namespace hbe
