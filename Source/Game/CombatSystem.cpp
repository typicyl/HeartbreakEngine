// Game/CombatSystem.cpp - faction-based health / damage / weapons implementation.
#include "Game/CombatSystem.h"

#include "Core/Log.h"
#include "Game/GameSystems.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace hbe::combat {

namespace {

// World-space center used as the aim/hit target for an entity. Character origins
// sit at the capsule bottom, so lift by half the capsule height to hit the torso;
// fall back to a ~torso height for plain movers.
glm::vec3 TargetCenter(const Scene& scene, entt::entity e) {
    const glm::vec3 p = glm::vec3(scene.WorldMatrix(e)[3]);
    const entt::registry& reg = scene.Registry();
    if (const auto* cc = reg.try_get<CharacterController>(e))
        return p + glm::vec3(0.0f, cc->height * 0.5f, 0.0f);
    return p + glm::vec3(0.0f, 0.9f, 0.0f);
}

// Ray (unit `dir`) vs sphere. Returns the entry distance in [0,maxT], or -1 on miss.
f32 RaySphere(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& center,
              f32 radius, f32 maxT) {
    const glm::vec3 oc = center - origin;
    const f32 tca = glm::dot(oc, dir);
    const f32 d2 = glm::dot(oc, oc) - tca * tca;
    const f32 r2 = radius * radius;
    if (d2 > r2) return -1.0f;
    const f32 thc = std::sqrt(std::max(0.0f, r2 - d2));
    f32 t = tca - thc;             // near intersection
    if (t < 0.0f) t = tca;         // origin inside the sphere -> hit at closest approach
    if (t < 0.0f || t > maxT) return -1.0f;
    return t;
}

} // namespace

f32 ApplyDamage(Scene& scene, const DamageEvent& ev, PhysicsWorld* physics) {
    if (ev.target == entt::null || ev.amount <= 0.0f) return 0.0f;
    entt::registry& reg = scene.Registry();
    // A DESPAWNED target, not merely a null one. Damage events travel through
    // schematics (an Entity pin is raw handle bits), through AI (a target sensed last
    // frame), and through the death queue - any of which can name an entity a shard
    // despawn destroyed since. try_get on an invalid handle is an ENTT_ASSERT in Debug
    // and an out-of-bounds sparse-set index in Release, so the guard belongs at this
    // chokepoint rather than at each of the callers.
    if (!reg.valid(ev.target)) return 0.0f;
    Health* h = reg.try_get<Health>(ev.target);
    if (!h || !h->alive || h->invincible) return 0.0f;
    if (h->invulnTimer > 0.0f && !ev.ignoreInvuln) return 0.0f;

    // Faction gate: an instigator that carries Health can only hurt a hostile
    // (or explicitly friendly-fire-enabled) target. Environmental damage passes
    // ignoreFaction and skips this.
    if (!ev.ignoreFaction && ev.instigator != entt::null && reg.valid(ev.instigator)) {
        if (const Health* ih = reg.try_get<Health>(ev.instigator)) {
            if (!Hostile(ih->faction, h->faction) && !h->friendlyFire) return 0.0f;
        }
    }

    h->current -= ev.amount;
    h->sinceDamage = 0.0f;
    h->invulnTimer = std::max(h->invulnTimer, h->hitInvuln);
    h->lastAttacker = ev.instigator;

    if (physics && ev.impulse > 0.0f) {
        const glm::vec3 d = glm::length(ev.direction) > 1e-4f ? glm::normalize(ev.direction)
                                                              : glm::vec3(0.0f, 0.2f, 0.0f);
        physics->AddImpulse(scene, ev.target, d * ev.impulse); // no-op if bodyless
    }

    if (h->current <= 0.0f) {
        h->current = 0.0f;
        h->alive = false; // combat::Update fires the one-shot death dispatch
    }
    return ev.amount;
}

void ApplyRadialDamage(Scene& scene, PhysicsWorld& physics, const glm::vec3& center,
                       f32 radius, f32 maxDamage, entt::entity instigator,
                       bool ignoreFaction) {
    if (radius <= 0.0f || maxDamage <= 0.0f) return;
    entt::registry& reg = scene.Registry();
    // Snapshot targets first: ApplyDamage doesn't structurally change the view, but
    // collecting up front keeps the blast independent of iteration order.
    std::vector<entt::entity> targets;
    for (auto e : reg.view<Health>()) targets.push_back(e);
    for (entt::entity e : targets) {
        if (e == instigator) continue; // never catch the shooter in its own blast
        const Health* h = reg.try_get<Health>(e);
        if (!h || !h->alive) continue;
        const glm::vec3 c = TargetCenter(scene, e);
        const glm::vec3 to = c - center;
        const f32 dist = glm::length(to);
        if (dist > radius) continue;
        // Line-of-sight: a wall between the blast center and the target blocks it.
        if (dist > 0.01f) {
            const glm::vec3 dir = to / dist;
            const f32 wall = physics.Raycast(center, dir, dist);
            if (wall < dist - 0.05f) continue; // occluded
        }
        DamageEvent ev;
        ev.target = e;
        ev.instigator = instigator;
        ev.amount = maxDamage * (1.0f - dist / radius); // linear falloff to the rim
        ev.point = c;
        ev.direction = dist > 0.01f ? to / dist : glm::vec3(0.0f, 1.0f, 0.0f);
        ev.impulse = 6.0f * (1.0f - dist / radius);
        ev.ignoreFaction = ignoreFaction;
        ApplyDamage(scene, ev, &physics);
    }
}

void Kill(Scene& scene, entt::entity e, bool respectInvuln) {
    if (e == entt::null || !scene.Registry().valid(e)) return;
    const Health* h = scene.Registry().try_get<Health>(e);
    if (!h) return;
    DamageEvent ev;
    ev.target = e;
    ev.amount = h->max + 1.0f;
    ev.ignoreInvuln = !respectInvuln;
    ev.ignoreFaction = true;
    ApplyDamage(scene, ev, nullptr);
}

void Heal(Scene& scene, entt::entity e, f32 amount) {
    if (amount <= 0.0f || e == entt::null || !scene.Registry().valid(e)) return;
    Health* h = scene.Registry().try_get<Health>(e);
    if (!h || !h->alive) return;
    h->current = std::min(h->max, h->current + amount);
}

bool IsAlive(const Scene& scene, entt::entity e) {
    if (e == entt::null || !scene.Registry().valid(e)) return false;
    const Health* h = scene.Registry().try_get<Health>(e);
    return h && h->alive;
}

bool TryFire(Scene& scene, PhysicsWorld& physics, entt::entity e, const glm::vec3& origin,
             const glm::vec3& dir) {
    entt::registry& reg = scene.Registry();
    Weapon* w = reg.try_get<Weapon>(e);
    if (!w) return false;
    if (w->cooldown > 0.0f || w->reloading > 0.0f) return false;
    if (w->maxAmmo >= 0 && w->ammo <= 0) {
        if (w->reserve > 0) w->reloading = w->reloadTime; // auto-reload
        return false;
    }
    if (glm::length(dir) < 1e-4f) return false;
    const glm::vec3 nd = glm::normalize(dir);

    if (w->kind == Weapon::Kind::Melee) {
        const f32 cosArc = std::cos(glm::radians(w->meleeArc));
        for (auto te : reg.view<Health, Transform>()) {
            if (te == e) continue;
            const Health* th = reg.try_get<Health>(te);
            if (!th || !th->alive) continue;
            const glm::vec3 c = TargetCenter(scene, te);
            const glm::vec3 to = c - origin;
            const f32 dist = glm::length(to);
            if (dist > w->range || dist < 1e-3f) continue;
            if (glm::dot(to / dist, nd) < cosArc) continue;
            DamageEvent ev;
            ev.target = te;
            ev.instigator = e;
            ev.amount = w->damage;
            ev.point = c;
            ev.direction = to / dist;
            ev.impulse = w->impulse;
            ApplyDamage(scene, ev, &physics);
        }
    } else { // Hitscan (Projectile reserved -> treated as hitscan for now)
        const f32 wallDist = physics.Raycast(origin, nd, w->range);
        f32 best = std::min(w->range, wallDist);
        entt::entity bestE = entt::null;
        for (auto te : reg.view<Health, Transform>()) {
            if (te == e) continue;
            const Health* th = reg.try_get<Health>(te);
            if (!th || !th->alive) continue;
            const f32 t = RaySphere(origin, nd, TargetCenter(scene, te), w->hitRadius, best);
            if (t >= 0.0f && t < best) {
                best = t;
                bestE = te;
            }
        }
        const glm::vec3 impact = origin + nd * best;
        if (w->radius > 0.0f) {
            // AoE from a weapon respects factions (don't wound allies/self); only
            // environmental explosions (direct ApplyRadialDamage callers) hurt everyone.
            ApplyRadialDamage(scene, physics, impact, w->radius, w->damage, e, false);
        } else if (bestE != entt::null) {
            DamageEvent ev;
            ev.target = bestE;
            ev.instigator = e;
            ev.amount = w->damage;
            ev.point = impact;
            ev.direction = nd;
            ev.impulse = w->impulse;
            ApplyDamage(scene, ev, &physics);
        }
    }

    if (w->maxAmmo >= 0) --w->ammo;
    w->cooldown = w->fireRate > 0.0f ? 1.0f / w->fireRate : 0.0f;
    // Firing makes noise the AI can hear (gunshots loud, melee faint) -> the
    // hearing pillar of stealth. Emitted at the shooter's position.
    game::EmitNoise(origin, w->kind == Weapon::Kind::Melee ? 0.4f : 1.6f);
    return true;
}

void Update(Scene& scene, f32 dt) {
    entt::registry& reg = scene.Registry();

    // Weapon cooldown + reload (independent of Health so unarmed actors are cheap).
    for (auto e : reg.view<Weapon>()) {
        Weapon& w = reg.get<Weapon>(e);
        w.cooldown = std::max(0.0f, w.cooldown - dt);
        if (w.reloading > 0.0f) {
            w.reloading -= dt;
            if (w.reloading <= 0.0f && w.maxAmmo >= 0) {
                const i32 need = w.maxAmmo - w.ammo;
                const i32 take = std::min(need, w.reserve);
                w.ammo += take;
                w.reserve -= take;
            }
        }
    }

    for (auto e : reg.view<Health>()) {
        Health& h = reg.get<Health>(e);
        h.invulnTimer = std::max(0.0f, h.invulnTimer - dt);
        if (h.alive) {
            h.sinceDamage += dt;
            if (h.regenRate > 0.0f && h.sinceDamage >= h.regenDelay && h.current < h.max)
                h.current = std::min(h.max, h.current + h.regenRate * dt);
            continue;
        }
        if (h.deathDispatched) continue;
        h.deathDispatched = true;

        if (!h.onDeathFlag.empty()) game::SetFlag(h.onDeathFlag, h.onDeathFlagValue);
        if (!h.onDeathObjective.empty()) game::CompleteObjective(h.onDeathObjective);

        game::DeathRec rec;
        if (!h.deathTag.empty()) rec.tag = h.deathTag;
        else if (const Name* n = reg.try_get<Name>(e)) rec.tag = n->value;
        rec.entity = static_cast<u32>(e);
        // A killer that was itself destroyed first - routine once shards despawn -
        // must not be laundered into the queue as raw bits: schematics read the
        // Instigator pin and hand it straight to try_get. Report "no instigator".
        rec.instigator = (h.lastAttacker != entt::null && reg.valid(h.lastAttacker))
                             ? static_cast<u32>(h.lastAttacker)
                             : 0xFFFFFFFFu;
        game::QueueDeath(rec);

        // Corpse: stop pathing + switch to the death clip (ragdoll is a future hook).
        if (auto* na = reg.try_get<NavigationAgent>(e)) na->hasTarget = false;
        if (h.deathClip >= 0) {
            if (auto* an = reg.try_get<Animator>(e)) {
                an->clip = h.deathClip;
                an->time = 0.0f;
                an->loop = false;
            }
        }
        // TODO(ragdoll): swap to an articulated Jolt body seeded with the last
        // DamageEvent impulse/direction instead of a canned death clip.
    }
}

} // namespace hbe::combat
