// Game/GameplaySystems.cpp - the gameplay update band (sequences sub-systems).
#include "Game/GameplaySystems.h"

#include "Core/Input.h"
#include "Game/AISystem.h"
#include "Game/CombatSystem.h"
#include "Game/GameSystems.h"
#include "Game/SpawnSystem.h"
#include "Renderer/Camera.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace hbe::gameplay {

void Update(Scene& scene, PhysicsWorld& physics, Renderer& renderer, const Input& input,
            const Camera& camera, f32 dt, bool uiCapturedPointer) {
    // Age the AI noise bus once per frame before anyone reads it (footsteps/gunfire
    // emitted this frame + last frame stay audible for their short TTL).
    game::TickNoises(dt);

    // 1) AI: sense the world, run each brain's FSM, set NavigationAgent targets,
    //    and fire at hostiles. Runs before nav so targets steer this frame.
    ai::Update(scene, physics, dt);

    // 2) Spawning / encounters: trigger spawns (prefab instantiation), track alive
    //    counts, fire cleared actions. Creates/destroys entities before combat reads.
    spawn::Update(scene, renderer, dt);
    spawn::UpdateEncounters(scene, dt);

    // 3) Combat: regen, i-frame timers, weapon cooldown/reload, death dispatch.
    combat::Update(scene, dt);

    // 4) Player fire: the first CharacterController that carries a Weapon fires
    //    from the camera on the attack button (cooldown gates the rate). The
    //    player capsule is bodyless, so the shot never self-hits.
    //
    //    Suppressed while the pointer owns a UI widget. Without this, every
    //    dialogue-choice click, pause-menu click and HUD click discharged the
    //    weapon - the click is consumed by the UI and ALSO fires the gun, because
    //    this branch reads the raw device with no notion of capture.
    entt::registry& reg = scene.Registry();
    if (!uiCapturedPointer && input.IsMouseDown(MouseButton::Left)) {
        for (auto e : reg.view<CharacterController, Weapon>()) {
            combat::TryFire(scene, physics, e, camera.Position(), camera.Forward());
            break; // one player
        }
    }
}

} // namespace hbe::gameplay
