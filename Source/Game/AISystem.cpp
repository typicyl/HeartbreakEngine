// Game/AISystem.cpp - perception + a compact hand-rolled behavior FSM.
//
// Each agent (Transform + AIPerception + AIBehavior + NavigationAgent): SENSE the
// nearest HOSTILE Health entity via a sight cone + PhysicsWorld::Raycast LoS and
// the game:: noise bus; integrate an awareness meter; TRANSITION its FSM state;
// then ACT by writing NavigationAgent.target (locomotion animates itself from the
// resulting agent velocity via motion matching) and firing its Weapon / melee via
// combat::. The AI never touches animation or the Transform directly except to
// face its target while attacking.
#include "Game/AISystem.h"

#include "Game/CombatSystem.h"
#include "Game/GameSystems.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace hbe::ai {

namespace {

glm::vec3 WorldPos(const Scene& scene, entt::entity e) {
    return glm::vec3(scene.WorldMatrix(e)[3]);
}

// Torso-height center for sight + aim (character origins sit at the feet).
glm::vec3 BodyCenter(const Scene& scene, entt::entity e) {
    const glm::vec3 p = WorldPos(scene, e);
    const entt::registry& reg = scene.Registry();
    if (const auto* cc = reg.try_get<CharacterController>(e))
        return p + glm::vec3(0.0f, cc->height * 0.5f, 0.0f);
    return p + glm::vec3(0.0f, 1.0f, 0.0f);
}

// The entity's world -Z (its facing).
glm::vec3 ForwardDir(const Scene& scene, entt::entity e) {
    const glm::vec3 f = glm::vec3(scene.WorldMatrix(e) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    const f32 len = glm::length(f);
    return len > 1e-5f ? f / len : glm::vec3(0.0f, 0.0f, -1.0f);
}

// Yaw the entity to look at a world point (horizontal only; -Z is forward).
void FaceTarget(Scene& scene, entt::entity e, const glm::vec3& targetPos) {
    Transform* t = scene.Registry().try_get<Transform>(e);
    if (!t) return;
    glm::vec3 d = targetPos - WorldPos(scene, e);
    d.y = 0.0f;
    if (glm::length(d) < 1e-3f) return;
    t->rotation = glm::quatLookAt(glm::normalize(d), glm::vec3(0.0f, 1.0f, 0.0f));
}

void AdvancePatrol(AIBehavior& beh) {
    const u32 n = static_cast<u32>(beh.patrolPoints.size());
    if (n <= 1) return;
    if (beh.patrolMode == 1) { // ping-pong
        if (beh.patrolForward) {
            if (beh.patrolIndex + 1 >= n) { beh.patrolForward = false; beh.patrolIndex = n - 2; }
            else ++beh.patrolIndex;
        } else {
            if (beh.patrolIndex == 0) { beh.patrolForward = true; beh.patrolIndex = 1; }
            else --beh.patrolIndex;
        }
    } else if (beh.patrolMode == 2) { // once, then hold at the end
        if (beh.patrolIndex + 1 < n) ++beh.patrolIndex;
    } else { // loop
        beh.patrolIndex = (beh.patrolIndex + 1) % n;
    }
}

// Candidate target: a live Health entity (hostility decided per agent by faction).
struct Cand {
    entt::entity e;
    glm::vec3 center;
    Faction fac;
};

} // namespace

void Update(Scene& scene, PhysicsWorld& physics, f32 dt) {
    entt::registry& reg = scene.Registry();
    auto agents = reg.view<Transform, AIPerception, AIBehavior, NavigationAgent>();
    if (agents.begin() == agents.end()) return; // no AI agents -> zero per-frame cost

    // Candidate targets = live Health entities. Reused across frames (gameplay is
    // single-threaded) so there's no per-frame heap allocation (matters in Debug).
    static std::vector<Cand> targets;
    targets.clear();
    for (auto te : reg.view<Health, Transform>()) {
        const Health& th = reg.get<Health>(te);
        if (!th.alive) continue;
        targets.push_back({te, BodyCenter(scene, te), th.faction});
    }

    for (auto e : agents) {
        AIPerception& per = reg.get<AIPerception>(e);
        AIBehavior& beh = reg.get<AIBehavior>(e);
        NavigationAgent& nav = reg.get<NavigationAgent>(e);
        Health* selfHealth = reg.try_get<Health>(e);
        const Faction selfFac = selfHealth ? selfHealth->faction : Faction::Enemy;

        // Dead: stop everything while actually dead.
        if (selfHealth && !selfHealth->alive) {
            beh.state = AIState::Dead;
            nav.hasTarget = false;
            continue;
        }
        // Revived (SetHealth put us back above 0): clear the stale Dead state so the
        // brain resumes instead of latching forever.
        if (beh.state == AIState::Dead) {
            beh.state = AIState::Idle;
            beh.stateTime = 0.0f;
        }

        if (!beh.spawnApplied) {
            beh.spawnApplied = true;
            if (beh.startAlerted) per.awareness = 1.0f; // scripted "already hunting"
        }

        const glm::vec3 eye = WorldPos(scene, e) + glm::vec3(0.0f, per.eyeHeight, 0.0f);
        const glm::vec3 fwd = ForwardDir(scene, e);
        const f32 cosHalf = std::cos(glm::radians(per.sightFovDeg * 0.5f));

        // --- SENSE: nearest hostile in the sight cone with a clear line of sight.
        entt::entity seen = entt::null;
        glm::vec3 seenCenter(0.0f);
        f32 seenDist = std::numeric_limits<f32>::max();
        for (const Cand& c : targets) {
            if (c.e == e || !Hostile(selfFac, c.fac)) continue;
            const glm::vec3 to = c.center - eye;
            const f32 dist = glm::length(to);
            if (dist > per.sightRange || dist < 1e-3f) continue;
            const glm::vec3 dir = to / dist;
            if (glm::dot(fwd, dir) < cosHalf) continue;             // outside the cone
            if (physics.Raycast(eye, dir, dist) < dist - 0.05f) continue; // wall between
            if (dist < seenDist) { seenDist = dist; seen = c.e; seenCenter = c.center; }
        }
        per.canSeeTarget = (seen != entt::null);

        // --- HEARING: any qualifying noise inside the loudness-scaled radius.
        per.heardSomething = false;
        for (const game::Noise& n : game::Noises()) {
            if (glm::length(n.pos - eye) <= per.hearingRadius * std::max(0.2f, n.loudness)) {
                per.heardPos = n.pos;
                per.heardSomething = true;
            }
        }

        // --- AWARENESS meter.
        const f32 prevAware = per.awareness;
        if (per.canSeeTarget) {
            const f32 prox = 1.0f + (1.0f - seenDist / per.sightRange); // closer = faster
            per.awareness += per.gainRate * prox * dt;
            per.knownTarget = seen;
            per.lastKnownPos = seenCenter;
            per.hasLastKnownPos = true;
            per.timeSinceSeen = 0.0f;
        } else if (per.heardSomething) {
            per.awareness += 0.5f * per.gainRate * dt;
            per.lastKnownPos = per.heardPos;
            per.hasLastKnownPos = true;
            per.timeSinceSeen += dt;
        } else {
            per.awareness -= per.decayRate * dt;
            per.timeSinceSeen += dt;
        }
        per.awareness = glm::clamp(per.awareness, 0.0f, 1.0f);

        if (prevAware < per.detectThreshold && per.awareness >= per.detectThreshold) {
            game::SpottedRec sr;
            sr.spotter = static_cast<u32>(e);
            sr.target =
                per.knownTarget != entt::null ? static_cast<u32>(per.knownTarget) : 0xFFFFFFFFu;
            game::QueueSpotted(sr);
        }

        // --- TRANSITIONS.
        const bool alerted = per.awareness >= per.detectThreshold;
        const bool fleeing = beh.fleeHealthFrac > 0.0f && selfHealth &&
                             selfHealth->current <= selfHealth->max * beh.fleeHealthFrac;
        beh.stateTime += dt;
        beh.attackCooldown = std::max(0.0f, beh.attackCooldown - dt);

        AIState next = beh.state;
        switch (beh.state) {
            case AIState::Idle:
            case AIState::Patrol:
                if (alerted) next = AIState::Chase;
                else if (per.heardSomething || per.awareness > 0.05f) next = AIState::Investigate;
                else next = beh.patrolPoints.empty() ? AIState::Idle : AIState::Patrol;
                break;
            case AIState::Investigate:
                if (alerted) next = AIState::Chase;
                else if (beh.stateTime >= beh.investigateTime)
                    next = beh.patrolPoints.empty() ? AIState::Idle : AIState::Patrol;
                break;
            case AIState::Chase:
                if (fleeing) next = AIState::Flee;
                else if (per.canSeeTarget && seenDist <= beh.attackRange) next = AIState::Attack;
                else if (!per.hasLastKnownPos) next = AIState::Search;
                else if (per.timeSinceSeen > per.loseSightGrace && nav.reached) next = AIState::Search;
                break;
            case AIState::Attack:
                if (fleeing) next = AIState::Flee;
                else if (!per.canSeeTarget || seenDist > beh.attackRange) next = AIState::Chase;
                break;
            case AIState::Search:
                if (alerted) next = AIState::Chase;
                else if (beh.stateTime >= beh.searchTime)
                    next = beh.patrolPoints.empty() ? AIState::Idle : AIState::Patrol;
                break;
            case AIState::Flee:
                if (!fleeing) next = AIState::Chase; // recovered
                break;
            default: break;
        }
        if (next != beh.state) {
            beh.prevState = beh.state;
            beh.state = next;
            beh.stateTime = 0.0f;
        }

        // --- ACT.
        switch (beh.state) {
            case AIState::Idle:
                nav.hasTarget = false;
                break;
            case AIState::Patrol: {
                if (beh.patrolPoints.empty()) { nav.hasTarget = false; break; }
                nav.target = beh.patrolPoints[beh.patrolIndex % beh.patrolPoints.size()];
                nav.hasTarget = true;
                if (nav.reached) {
                    beh.waitTimer -= dt;
                    if (beh.waitTimer <= 0.0f) {
                        beh.waitTimer = beh.waitAtPoint;
                        AdvancePatrol(beh);
                    }
                }
                break;
            }
            case AIState::Investigate:
            case AIState::Chase:
            case AIState::Search:
                if (per.hasLastKnownPos) { nav.target = per.lastKnownPos; nav.hasTarget = true; }
                else nav.hasTarget = false;
                break;
            case AIState::Attack: {
                nav.hasTarget = false; // stand and fight
                const entt::entity victim = seen != entt::null ? seen : per.knownTarget;
                if (victim != entt::null && reg.valid(victim)) {
                    FaceTarget(scene, e, BodyCenter(scene, victim));
                    const bool useWeapon = beh.useWeapon && reg.all_of<Weapon>(e);
                    if (useWeapon) {
                        combat::TryFire(scene, physics, e, eye, BodyCenter(scene, victim) - eye);
                    } else if (beh.attackCooldown <= 0.0f) {
                        beh.attackCooldown = beh.attackInterval;
                        combat::DamageEvent ev;
                        ev.target = victim;
                        ev.instigator = e;
                        ev.amount = beh.attackDamage;
                        ev.direction = glm::normalize(BodyCenter(scene, victim) - eye);
                        ev.impulse = 2.0f;
                        combat::ApplyDamage(scene, ev, &physics);
                    }
                }
                break;
            }
            case AIState::Flee: {
                glm::vec3 dest;
                const glm::vec3 threat = per.hasLastKnownPos ? per.lastKnownPos : eye;
                if (!beh.patrolPoints.empty()) {
                    f32 best = -1.0f;
                    dest = beh.patrolPoints[0];
                    for (const glm::vec3& p : beh.patrolPoints) {
                        const f32 d = glm::length(p - threat);
                        if (d > best) { best = d; dest = p; }
                    }
                } else {
                    const glm::vec3 self = WorldPos(scene, e);
                    const glm::vec3 dir =
                        per.hasLastKnownPos && glm::length(self - threat) > 1e-3f
                            ? glm::normalize(self - threat)
                            : glm::vec3(0.0f, 0.0f, 1.0f);
                    dest = self + dir * 10.0f;
                }
                nav.target = dest;
                nav.hasTarget = true;
                break;
            }
            default: break;
        }
    }
}

} // namespace hbe::ai
