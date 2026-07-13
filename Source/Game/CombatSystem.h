// Game/CombatSystem.h - faction-based health / damage / weapons.
//
// Combat is DATA-driven and faction-based, not type-based: the player and every
// enemy carry the same `Health` component (Components.h) and differ only by
// `Health.faction`. combat::ApplyDamage is the single choke point that enforces
// faction/invuln/invincible rules and flags death; combat::Update ticks regen,
// i-frame timers, weapon cooldowns and fires the one-shot death dispatch. Weapons
// (`Weapon`) are a capability an actor carries; combat::TryFire resolves hits.
//
// Hitscan reality: PhysicsWorld::Raycast returns a DISTANCE only (no entity) and
// characters are bodyless CharacterVirtual capsules that a Jolt ray never hits, so
// character hits are resolved with a gameplay-side ray-vs-sphere over Health
// entities; the physics raycast is used only for wall/line-of-fire occlusion.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace hbe {

class Scene;
class PhysicsWorld;

namespace combat {

// A single application of damage. Entities may be null (environmental damage).
struct DamageEvent {
    entt::entity target     = entt::null;
    entt::entity instigator = entt::null;       // who caused it (faction + attribution)
    f32          amount     = 0.0f;
    glm::vec3    point{0.0f};                    // world hit point (impulse origin / VFX)
    glm::vec3    direction{0.0f, 0.0f, -1.0f};   // normalized travel dir (knockback)
    f32          impulse       = 0.0f;
    bool         ignoreInvuln  = false;          // scripted Kill bypasses i-frames
    bool         ignoreFaction = false;          // env/explosions hurt everyone
};

// DIRECT damage to one entity. Applies faction/invuln/invincible filtering, mutates
// Health, stamps i-frames + resets the regen delay, applies knockback (if `physics`
// and impulse), and on the alive->dead edge sets alive=false (combat::Update owns
// the death DISPATCH). Returns the damage actually dealt (0 = filtered/dead/no-op).
f32 ApplyDamage(Scene& scene, const DamageEvent& ev, PhysicsWorld* physics = nullptr);

// RADIAL damage: every live Health entity whose center is within `radius` of
// `center` takes falloff-scaled damage (linear to 0 at the rim), line-of-sight
// gated by a wall raycast from `center` (walls block the blast).
void ApplyRadialDamage(Scene& scene, PhysicsWorld& physics, const glm::vec3& center,
                       f32 radius, f32 maxDamage, entt::entity instigator,
                       bool ignoreFaction = true);

// Scripted instant kill (bypasses i-frames unless `respectInvuln`).
void Kill(Scene& scene, entt::entity e, bool respectInvuln = false);

// Adds `amount` hp to a live entity (clamped to max). No-op on dead/missing.
void Heal(Scene& scene, entt::entity e, f32 amount);

// Is `e` a live, damageable entity?
bool IsAlive(const Scene& scene, entt::entity e);

// Fire `e`'s Weapon this frame if off cooldown and ammo. Hitscan traces from
// `origin` along `dir` (player: camera; AI: muzzle->target); Melee sweeps a forward
// arc from the actor. All hits route through ApplyDamage. Returns true if a shot
// was expended (cooldown started). Also auto-reloads when the mag is empty.
bool TryFire(Scene& scene, PhysicsWorld& physics, entt::entity e,
             const glm::vec3& origin, const glm::vec3& dir);

// Per-frame health/weapon tick + death dispatch. Called by gameplay::Update while
// the simulation runs. Ticks invuln timers, regen, weapon cooldown/reload, and on
// the alive->dead edge (once): sets the death flag/objective, queues the OnDeath
// schematic event, stops the NavigationAgent, and switches to the death clip.
void Update(Scene& scene, f32 dt);

} // namespace combat
} // namespace hbe
