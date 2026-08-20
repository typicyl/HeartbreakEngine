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

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>

namespace hbe {

class Scene;
class Renderer;

namespace spawn {

// Instantiate a `.hbvfx` particle effect at a world transform (loaded + cached from Assets/). Adds
// a Transform + ParticleEmitter entity; a one-shot (non-looping) effect also gets an EffectLifetime
// so it self-destroys when finished. Returns the created entity (entt::null on failure). This is the
// immediate form used by the editor preview + the drain of game::SpawnEffect. `name`'s `.hbvfx`
// extension is optional.
entt::entity SpawnEffect(Scene& scene, const std::string& name, const glm::mat4& transform);

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
