// Interaction/Pick.cpp - the unified pick pass (see Pick.h for why it exists).
#include "Interaction/Pick.h"

#include "Core/Input.h"
#include "Core/Log.h"
#include "Physics/PhysicsWorld.h" // SelfTest only (real occlusion, not a stub)
#include "Renderer/Camera.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/UISystem.h"
#include "UI/UIWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace hbe::interact {

namespace {

// Local-space AABB of an entity, or a sphere-ish box of `r` when it has none.
// Bounds-less content is the common case for an authored NPC marker, and it must
// stay reachable - this is deliberately not "no AABB, no interaction".
//
// A CharacterController with no AABB gets its CAPSULE instead of `r`. That is the
// realistic NPC: `range * 0.15` produced a 0.75 m cube centred on the rig PIVOT,
// i.e. at the feet with half of it underground, so aiming at a character's head or
// chest missed the aim-first branch entirely. (Character rigs also carry a real
// AABB on the root now - see character::Instantiate - so this is the belt to that
// braces: it covers a plain CharacterController NPC with no .hbchar.)
void LocalBounds(const entt::registry& reg, entt::entity e, f32 r, glm::vec3& lo,
                 glm::vec3& hi) {
    if (const AABB* bb = reg.try_get<AABB>(e)) {
        lo = glm::min(bb->min, bb->max);
        hi = glm::max(bb->min, bb->max);
        // A zero-volume AABB (an authored marker with no mesh) would be un-hittable.
        const glm::vec3 pad = glm::max(glm::vec3(0.0f), glm::vec3(1e-4f) - (hi - lo));
        lo -= pad * 0.5f;
        hi += pad * 0.5f;
        return;
    }
    if (const CharacterController* cc = reg.try_get<CharacterController>(e)) {
        // Same convention as PhysicsWorld: the capsule is CENTRED on the entity.
        const f32 cr = glm::max(cc->radius, 0.05f);
        const f32 hh = glm::max(cc->height * 0.5f, cr);
        lo = {-cr, -hh, -cr};
        hi = {cr, hh, cr};
        return;
    }
    lo = glm::vec3(-r);
    hi = glm::vec3(r);
}

// Ray vs a vertical capsule (centre `c`, cylinder half-height `hh`, radius `rad`).
// Returns the ENTRY parameter, 0 when the ray starts inside. Used for the
// CharacterController occluders, which own no physics body to cast against.
bool RayCapsuleY(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& c,
                 f32 hh, f32 rad, f32 maxT, f32& outT) {
    // Infinite cylinder about the capsule's Y axis, then cap with the two spheres.
    const glm::vec3 p = origin - c;
    const f32 a = dir.x * dir.x + dir.z * dir.z;
    f32 best = std::numeric_limits<f32>::max();
    const auto consider = [&](f32 t, f32 y) {
        if (t < 0.0f || t > maxT) return;
        if (std::abs(y) > hh) return;
        best = glm::min(best, t);
    };
    if (a > 1e-12f) {
        const f32 b = 2.0f * (p.x * dir.x + p.z * dir.z);
        const f32 cc = p.x * p.x + p.z * p.z - rad * rad;
        const f32 disc = b * b - 4.0f * a * cc;
        if (disc >= 0.0f) {
            const f32 sq = std::sqrt(disc);
            consider((-b - sq) / (2.0f * a), p.y + dir.y * ((-b - sq) / (2.0f * a)));
            consider((-b + sq) / (2.0f * a), p.y + dir.y * ((-b + sq) / (2.0f * a)));
        }
    }
    // Hemispherical caps.
    for (const f32 cy : {-hh, hh}) {
        const glm::vec3 q = p - glm::vec3(0.0f, cy, 0.0f);
        const f32 b = 2.0f * glm::dot(q, dir);
        const f32 cc = glm::dot(q, q) - rad * rad;
        const f32 disc = b * b - 4.0f * cc; // dir is unit -> a == 1
        if (disc < 0.0f) continue;
        const f32 sq = std::sqrt(disc);
        for (const f32 t : {(-b - sq) * 0.5f, (-b + sq) * 0.5f}) {
            if (t < 0.0f || t > maxT) continue;
            const f32 y = p.y + dir.y * t;
            if ((cy < 0.0f && y > -hh) || (cy > 0.0f && y < hh)) continue;
            best = glm::min(best, t);
        }
    }
    // Starting INSIDE the capsule: entry is 0 (the caller's self-exemption is what
    // keeps the player's own capsule from blocking a first-person ray).
    {
        const f32 y = glm::clamp(p.y, -hh, hh);
        if (glm::distance(p, glm::vec3(0.0f, y, 0.0f)) <= rad) best = 0.0f;
    }
    if (best == std::numeric_limits<f32>::max()) return false;
    outT = best;
    return true;
}

} // namespace

PointerMode ResolvePointer(const PointerInputs& in) {
    PointerMode out;

    // THE RETICLE. Two reasons a pointer becomes the screen centre:
    //
    //  1. THE CURSOR IS LOCKED (first-person gameplay). There is no usable cursor
    //     at all - while locked, WM_MOUSEMOVE routes to OnMouseLockedDelta and
    //     never OnMouseMove, so MouseX/MouseY are frozen at wherever the cursor
    //     last was when it was free.
    //  2. A GAMEPAD IS DRIVING, in ANY cursor state. A pad moves no mouse, and
    //     ui::UIFocus deliberately never lands focus on a world-space canvas
    //     (their pixel spaces are unrelated to the screen's, so navigation could
    //     jump to an off-screen page). Aim is therefore a pad's ONLY route to a
    //     world page, and without this a pad user with a free cursor - a dialogue
    //     choice, a pause overlay - could not touch one at all.
    //
    // ...except when a SCREEN focus ring is live. That is the pad navigating a
    // menu, and screen space beats world space: otherwise one press of the
    // activate button would fire the focused menu button AND a world page that
    // happened to be under the crosshair behind it.
    // ...and the exception applies to BOTH reasons, not just the pad one. It used
    // to read `padActive && !screenFocusActive`, so a LOCKED cursor re-enabled the
    // reticle unconditionally - and locked-cursor gameplay is exactly where a HUD
    // focus ring lives. One press of the Interact-bound pad button then both
    // activated the focused HUD widget (ui::UpdateNavigation) and pressed the
    // reticle-aimed world page: the precise double-fire this guard exists to stop.
    out.reticle = !in.external && !in.screenFocusActive &&
                  (in.cursorLocked || in.padActive);
    out.useInteractAction = out.reticle;

    const glm::vec2 cursor =
        in.external ? in.externalNorm
                    : (in.cursorLocked ? glm::vec2(-1.0f) : in.cursorNorm);
    out.worldPointer = out.reticle ? glm::vec2(0.5f, 0.5f) : cursor;
    out.screenPointer = cursor;
    return out;
}

bool RayEntityBox(Scene& scene, entt::entity e, const glm::vec3& origin,
                  const glm::vec3& dir, f32 maxT, f32 fallbackRadius, f32& outT) {
    const entt::registry& reg = scene.Registry();
    if (!reg.valid(e)) return false;
    glm::vec3 lo(0.0f), hi(0.0f);
    LocalBounds(reg, e, fallbackRadius, lo, hi);

    // Ray into the entity's own space. An affine transform preserves the ray
    // PARAMETER, so the local t IS the world t - no rescaling, and this works for
    // rotated and non-uniformly scaled objects alike (that is the whole point of
    // an OBB test over a world-AABB test).
    const glm::mat4 inv = glm::inverse(scene.WorldMatrix(e));
    const glm::vec3 lo0 = glm::vec3(inv * glm::vec4(origin, 1.0f));
    const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));

    f32 tmin = 0.0f, tmax = maxT;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(ld[a]) < 1e-9f) {
            if (lo0[a] < lo[a] || lo0[a] > hi[a]) return false; // parallel and outside
            continue;
        }
        const f32 invD = 1.0f / ld[a];
        f32 t0 = (lo[a] - lo0[a]) * invD;
        f32 t1 = (hi[a] - lo0[a]) * invD;
        if (t0 > t1) std::swap(t0, t1);
        tmin = glm::max(tmin, t0);
        tmax = glm::min(tmax, t1);
        if (tmin > tmax) return false;
    }
    outT = tmin;
    return true;
}

f32 PointEntityBoxDistance(Scene& scene, entt::entity e, const glm::vec3& p,
                           f32 fallbackRadius) {
    const entt::registry& reg = scene.Registry();
    if (!reg.valid(e)) return std::numeric_limits<f32>::max();
    glm::vec3 lo(0.0f), hi(0.0f);
    LocalBounds(reg, e, fallbackRadius, lo, hi);
    const glm::mat4 M = scene.WorldMatrix(e);
    const glm::vec3 lp = glm::vec3(glm::inverse(M) * glm::vec4(p, 1.0f));
    const glm::vec3 clamped = glm::clamp(lp, lo, hi);
    // Measure in WORLD space: the local-space delta would be wrong under scale.
    return glm::distance(p, glm::vec3(M * glm::vec4(clamped, 1.0f)));
}

Hit Pick(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
         const OccludeFn& occlude, const AcceptFn& accept, const Params& params,
         ui::UIContext* ctx) {
    Hit out;
    entt::registry& reg = scene.Registry();
    const f32 maxRange = glm::max(params.maxRange, 0.0f);

    const bool havePointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f &&
                             pointerNorm.x <= 1.0f && pointerNorm.y <= 1.0f;

    // ---- the ray ------------------------------------------------------------
    glm::vec3 origin(0.0f), dir(0.0f, 0.0f, 1.0f);
    if (havePointer) {
        camera.ScreenRay(pointerNorm, origin, dir);
        out.hasRay = true;
        out.origin = origin;
        out.dir = dir;
    }

    // ---- ONE occlusion cast -------------------------------------------------
    // This single query is the entire cost of occlusion for both pages and
    // objects. Doing it per candidate would be the same query N times.
    f32 wallT = maxRange;
    if (havePointer && occlude) {
        const Block b = occlude(origin, dir, maxRange);
        ++out.raycasts;
        out.wallDistance = b.hit ? b.distance : maxRange;
        out.wallEntity = b.entity;
        wallT = out.wallDistance;
    } else {
        out.wallDistance = maxRange;
    }
    // CHARACTERS OCCLUDE TOO. A CharacterController is a JPH::CharacterVirtual with
    // no system body, so the physics query above is blind to every NPC in the game
    // - without this, the reticle picks a wall page straight through an NPC's
    // chest. Cheap (a handful of capsules, no physics call) and it can honour an
    // exemption the physics path cannot: the player's OWN capsule, which a
    // third-person camera sits behind.
    if (havePointer && params.charactersOcclude) {
        for (const entt::entity e : reg.view<Transform, CharacterController>()) {
            if (e == params.anchorEntity) continue;
            const CharacterController& cc = reg.get<CharacterController>(e);
            const f32 rad = glm::max(cc.radius, 0.05f);
            const f32 hh = glm::max(cc.height * 0.5f - rad, 0.0f);
            const glm::vec3 centre = glm::vec3(scene.WorldMatrix(e)[3]);
            f32 t = 0.0f;
            if (!RayCapsuleY(origin, dir, centre, hh, rad, wallT, t)) continue;
            if (t <= 0.0f) continue; // the ray starts inside: not an occluder
            if (t < wallT) {
                wallT = t;
                out.wallDistance = t;
                out.wallEntity = e;
            }
        }
    }
    const f32 horizon = wallT + params.skin;

    // ---- pages --------------------------------------------------------------
    ui::PagePick page;
    if (havePointer) {
        for (const entt::entity e : reg.view<UISurface>()) { (void)e; ++out.pagesTested; }
        // `out.wallEntity` goes with the horizon: a page must not be occluded by
        // the prop it is BOLTED TO (a terminal, a tablet, a recessed screen behind
        // a bezel), exactly as an Interactable is not occluded by its own collider
        // in the fallback below.
        page = ui::PickWorldPage(scene, origin, dir, horizon, ctx, out.wallEntity);
        if (page.hit && page.distance <= maxRange) {
            out.kind = Hit::Kind::Page;
            out.entity = page.canvas;
            out.distance = page.distance;
            out.canvasPx = page.px;
            out.point = page.point;
            out.aimed = true;
        }
    }

    if (!params.considerObjects) return out;

    // ---- objects: AIM FIRST -------------------------------------------------
    // Candidates are ray-tested against their world OBB, NOT against a physics
    // body: most Interactables have no RigidBody at all (the component requires
    // none), so a naive "raycast physics and see what you hit" would silently stop
    // prompting on existing content. Occlusion still works, because the WALL has
    // the collider: the physics ray returns it nearer than the object's box.
    entt::entity bestObj = entt::null;
    f32 bestT = out.kind == Hit::Kind::Page ? out.distance : maxRange;
    if (havePointer) {
        for (const entt::entity e : reg.view<Interactable>()) {
            const Interactable& ia = reg.get<Interactable>(e);
            if (accept && !accept(e)) continue;
            ++out.objectsTested;
            const f32 fr = glm::max(ia.range * 0.15f, 1e-3f);
            f32 t = 0.0f;
            if (!RayEntityBox(scene, e, origin, dir, bestT, fr, t)) continue;
            if (t > horizon) continue; // behind something solid
            // PROXIMITY IS A GATE, NOT THE SELECTOR. Aim decides WHICH; range still
            // decides WHETHER, so nothing reachable today becomes unreachable. The
            // gate measures to the BOX, so a large object no longer fails at
            // contact the way centre-to-pivot did.
            if (params.hasAnchor) {
                if (PointEntityBoxDistance(scene, e, params.anchor, fr) > ia.range)
                    continue;
            } else if (t > ia.range) {
                // NO ANCHOR (a standalone caller, or a scene with no
                // CharacterController): `range` was ignored outright, so every
                // Interactable within maxRange (100 m) under the reticle was a
                // candidate. Measure along the RAY instead - it is the only
                // distance that exists here, and it keeps `range` meaningful.
                continue;
            }
            bestT = t;
            bestObj = e;
        }
    }
    if (bestObj != entt::null) {
        out.kind = Hit::Kind::Object;
        out.entity = bestObj;
        out.distance = bestT;
        out.canvasPx = glm::vec2(0.0f);
        out.point = origin + dir * bestT;
        out.aimed = true;
        return out;
    }
    // ---- objects: PROXIMITY FALLBACK ---------------------------------------
    // Nothing under the reticle. Keep "walk up to an NPC and press E" working -
    // but occlusion-filtered, so the through-a-wall prompt is gone in this branch
    // too. Candidates are walked nearest-first and each costs at most one cast.
    //
    // A PAGE DOES NOT SILENCE THIS. It used to: `if (out.kind != None) return`
    // sat here, and page candidacy has no interactivity test and (by default) no
    // range limit - so a purely decorative diegetic screen 40 m down the corridor
    // removed the "[E] Talk" prompt from an NPC standing 1 m away. The pass's own
    // rule settles it instead: NEAREST WINS. The page keeps the pick unless a
    // proximity candidate is closer TO THE PLAYER than the page's hit point is.
    if (!params.hasAnchor) return out;
    const f32 pageAnchorDist = out.kind == Hit::Kind::Page
                                   ? glm::distance(params.anchor, out.point)
                                   : std::numeric_limits<f32>::max();
    struct Cand { entt::entity e; f32 d; };
    static thread_local std::vector<Cand> cands;
    cands.clear();
    for (const entt::entity e : reg.view<Interactable>()) {
        const Interactable& ia = reg.get<Interactable>(e);
        if (accept && !accept(e)) continue;
        ++out.objectsTested;
        const f32 fr = glm::max(ia.range * 0.15f, 1e-3f);
        const f32 d = PointEntityBoxDistance(scene, e, params.anchor, fr);
        // Farther from the player than the page that already won: it cannot take
        // the pick, so it is not worth an occlusion cast either.
        if (d <= ia.range && d < pageAnchorDist) cands.push_back({e, d});
    }
    if (cands.empty()) return out; // the page (if any) keeps it
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.d != b.d) return a.d < b.d;
        return a.e < b.e; // deterministic, never entt pool order
    });
    u32 casts = 0;
    for (const Cand& c : cands) {
        bool visible = true;
        if (occlude && casts < params.maxFallbackCasts) {
            const glm::mat4 M = scene.WorldMatrix(c.e);
            glm::vec3 lo(0.0f), hi(0.0f);
            const f32 fr = glm::max(reg.get<Interactable>(c.e).range * 0.15f, 1e-3f);
            LocalBounds(reg, c.e, fr, lo, hi);
            const glm::vec3 target =
                glm::vec3(M * glm::vec4((lo + hi) * 0.5f, 1.0f));
            const glm::vec3 rel = target - params.anchor;
            const f32 len = glm::length(rel);
            if (len > 1e-4f) {
                const Block b = occlude(params.anchor, rel / len, len);
                ++casts;
                ++out.raycasts;
                // The candidate's OWN collider is not an occluder of itself.
                visible = !b.hit || b.entity == c.e || b.distance >= len - params.skin;
            }
        }
        if (!visible) continue;
        out.kind = Hit::Kind::Object;
        out.entity = c.e;
        out.distance = c.d;
        // A page may have won the ray above; taking the pick means taking ALL of
        // it, or the caller would feed page pixels to a canvas that no longer owns
        // the pointer (a world button left hovered behind an NPC prompt).
        out.canvasPx = glm::vec2(0.0f);
        out.point = glm::vec3(0.0f);
        out.aimed = false;
        break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// --test-uipick
// ---------------------------------------------------------------------------
namespace {

int g_fails = 0;
void Fail(const std::string& why) {
    ++g_fails;
    std::printf("  FAIL: %s\n", why.c_str());
}
void Check(bool ok, const std::string& why) {
    if (!ok) Fail(why);
}

// A world page: a UICanvas plus the UISurface quad UpdateWorldSurfaces would have
// made for it (the surface needs no renderer - only its Transform is picked).
entt::entity MakePage(Scene& s, glm::vec3 pos, glm::vec3 eulerDeg, glm::vec3 scale,
                      f32 worldWidth, f32 refW, f32 refH) {
    entt::registry& reg = s.Registry();
    const entt::entity c = s.CreateEntity("page");
    Transform t;
    t.position = pos;
    t.rotation = glm::quat(glm::radians(eulerDeg));
    t.scale = scale;
    reg.emplace_or_replace<Transform>(c, t);
    UICanvas uc;
    uc.worldSpace = true;
    uc.visible = true;
    uc.refWidth = refW;
    uc.refHeight = refH;
    uc.worldWidth = worldWidth;
    reg.emplace<UICanvas>(c, uc);

    const entt::entity surf = s.CreateEntity("__uiSurface");
    reg.emplace<UISurface>(surf, UISurface{c});
    reg.emplace<Parent>(surf, Parent{c});
    Transform st;
    st.scale = {worldWidth, 1.0f, worldWidth * refH / refW};
    reg.emplace_or_replace<Transform>(surf, st);
    reg.get<UICanvas>(c).surface = surf;
    return c;
}

entt::entity MakeStaticBox(Scene& s, glm::vec3 pos, glm::vec3 half) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity("wall");
    Transform t;
    t.position = pos;
    reg.emplace_or_replace<Transform>(e, t);
    RigidBody rb;
    rb.shape = RigidBody::Shape::Box;
    rb.motion = RigidBody::Motion::Static;
    rb.halfExtents = half;
    reg.emplace<RigidBody>(e, rb);
    return e;
}

// Camera at `eye` looking at `at`, 60 deg, 1:1 aspect so screen centre is exactly
// the forward axis and the maths in the test is hand-checkable. `up` MUST NOT be
// parallel to the view direction (a top-down camera needs an explicit one, or
// glm::lookAtRH degenerates to NaN).
constexpr f32 kNear = 0.05f;
Camera MakeCam(glm::vec3 eye, glm::vec3 at, glm::vec3 up = {0, 1, 0}) {
    Camera c;
    c.SetPerspective(60.0f, 1.0f, kNear, 500.0f);
    c.LookAt(eye, at, up);
    return c;
}
// Screen right = +X, screen up = -Z for the top-down rig used below.
const glm::vec3 kTopDownUp{0.0f, 0.0f, -1.0f};

// A centre-anchored Button. With no `canvas` it is a legacy canvas-less root (the
// project canvas); with one it lays out inside that canvas's own pixel space.
entt::entity MakeButton(Scene& s, glm::vec2 offset, glm::vec2 size,
                        entt::entity canvas = entt::null) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity("btn");
    UIElement el;
    el.type = UIElement::Type::Button;
    el.anchorMin = el.anchorMax = {0.5f, 0.5f};
    el.pivot = {0.5f, 0.5f};
    el.offset = offset;
    el.size = size;
    reg.emplace<UIElement>(e, std::move(el));
    if (canvas != entt::null) reg.emplace<Parent>(e, Parent{canvas});
    return e;
}

// One frame with the left button held (so WasMousePressed sees the down-edge).
void Press(Input& in, bool down) {
    in.NewFrame();
    if (down) in.OnMouseButton(MouseButton::Left, true);
}

OccludeFn PhysicsOccluder(const PhysicsWorld& phys) {
    return [&phys](const glm::vec3& o, const glm::vec3& d, f32 m) {
        const PhysicsWorld::RayHit h = phys.RaycastDetailed(o, d, m);
        Block b;
        b.hit = h.hit;
        b.distance = h.distance;
        b.entity = h.entity;
        return b;
    };
}

} // namespace

bool SelfTest() {
    g_fails = 0;
    const glm::vec2 centre(0.5f, 0.5f);
    Params P;
    P.maxRange = 100.0f;
    P.considerObjects = false;

    // 1. FLAT PAGE, HAND-COMPUTED PIXEL. A 2 m wide, 512x512 page lying in the XZ
    //    plane at the origin; the camera looks straight down at it. Screen centre
    //    must land on the exact canvas centre, and an off-centre pointer must land
    //    where the plane maths says.
    {
        Scene s;
        const entt::entity c = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        Hit h = Pick(s, cam, centre, {}, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page && h.entity == c, "1: centre ray missed the page");
        Check(glm::distance(h.canvasPx, glm::vec2(256.0f, 256.0f)) < 1e-2f,
              "1: centre of a flat page is not the centre pixel");
        // Camera::ScreenRay starts at the NEAR PLANE, not the eye.
        Check(std::abs(h.distance - (5.0f - kNear)) < 1e-3f, "1: page distance wrong");

        // Off-centre, hand-computed: at 60 deg fovY and aspect 1, the half-extent of
        // the ground plane 5 m below is 5*tan(30) = 2.8868 m. A pointer at x=0.65 is
        // ndc.x = 0.3, so the hit is 0.3*2.8868 = 0.866 m to the +X side, which on a
        // 2 m wide page is u = 0.866/2 + 0.5.
        const f32 halfExtent = 5.0f * std::tan(glm::radians(30.0f));
        const f32 wx = halfExtent * 0.3f;
        h = Pick(s, cam, {0.65f, 0.5f}, {}, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page, "1b: off-centre ray missed");
        const f32 wantU = (wx / 2.0f) + 0.5f;
        Check(std::abs(h.canvasPx.x - wantU * 512.0f) < 0.5f,
              "1b: off-centre pixel wrong (x)");
        Check(std::abs(h.canvasPx.y - 256.0f) < 0.5f, "1b: off-centre pixel wrong (y)");
    }

    // 2. OCCLUSION. The same page with a static box between it and the camera is
    //    NOT clickable; clearing UICanvas::occlude makes it clickable again;
    //    removing the wall makes it clickable again.
    {
        Scene s;
        PhysicsWorld phys;
        const entt::entity c = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        MakeStaticBox(s, {0, 2.5f, 0}, {2.0f, 0.25f, 2.0f});
        // Bodies are created lazily in Update, so a frame must tick before any
        // raycast is meaningful. (Update creates/syncs bodies even while paused.)
        phys.Update(s, 1.0f / 60.0f);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        const OccludeFn occ = PhysicsOccluder(phys);

        Hit h = Pick(s, cam, centre, occ, {}, P, nullptr);
        Check(h.kind == Hit::Kind::None,
              "2: a page BEHIND A WALL is still clickable (occlusion is not working)");
        Check(h.wallDistance < 3.0f, "2: the occlusion cast did not find the wall");

        s.Registry().get<UICanvas>(c).occlude = false;
        h = Pick(s, cam, centre, occ, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page, "2b: occlude=false did not opt out of occlusion");
        s.Registry().get<UICanvas>(c).occlude = true;

        // And with no occluder function at all (the headless / no-physics caller).
        h = Pick(s, cam, centre, {}, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page, "2c: no occluder should mean no occlusion");
    }

    // 3. TWO OVERLAPPING PAGES: exactly the NEARER one receives the pointer, and
    //    the far one is not merely lower priority - it is ABSENT from the result.
    {
        Scene s;
        const entt::entity far_ = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const entt::entity near_ = MakePage(s, {0, 1, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 256, 256);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        ui::PointerState ps;
        ui::ComputeWorldPointers(s, cam, centre, ps);
        Check(ps.worldCanvasPx.size() == 1,
              "3: overlapping pages both received the pointer (" +
                  std::to_string(ps.worldCanvasPx.size()) + " entries)");
        Check(ps.worldCanvasPx.count(static_cast<u32>(near_)) == 1,
              "3: the NEARER page did not get the pointer");
        Check(ps.worldCanvasPx.count(static_cast<u32>(far_)) == 0,
              "3: the FARTHER page got the pointer");
        // Declaration order must not decide it: build the same pair the other way.
        Scene s2;
        const entt::entity nearFirst =
            MakePage(s2, {0, 1, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 256, 256);
        MakePage(s2, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Hit h = Pick(s2, cam, centre, {}, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page && h.entity == nearFirst,
              "3b: nearest-wins depends on entity pool order");
    }

    // 4. ROTATED + NON-UNIFORMLY SCALED PAGE. The regression case for the plane
    //    normal: a page pitched under a parent-style non-uniform scale used to
    //    solve a DIFFERENT plane and pick offset from where it is drawn. The
    //    answer is cross-checked against an independent brute-force solve of the
    //    SAME quad from its four transformed corners.
    {
        Scene s;
        // A SHEARED composition: a non-uniformly scaled mount with a rotated page
        // under it. `M * (0,1,0,0)` is NOT the plane normal here.
        entt::registry& reg = s.Registry();
        const entt::entity mount = s.CreateEntity("mount");
        Transform mt;
        mt.scale = {2.0f, 1.0f, 0.5f};
        mt.rotation = glm::quat(glm::radians(glm::vec3(0.0f, 25.0f, 0.0f)));
        reg.emplace_or_replace<Transform>(mount, mt);
        const entt::entity c =
            MakePage(s, {1.0f, 0.5f, -0.5f}, {35.0f, 40.0f, 15.0f}, {1.0f, 1.0f, 1.0f},
                     1.5f, 800, 600);
        reg.emplace<Parent>(c, Parent{mount});

        // Independent solve of the SAME quad from its transformed corners - no
        // shared code with the picker beyond the world matrix itself.
        const entt::entity surf = reg.get<UICanvas>(c).surface;
        const glm::mat4 M = s.WorldMatrix(surf);
        const glm::vec3 o = glm::vec3(M * glm::vec4(-0.5f, 0, -0.5f, 1));
        const glm::vec3 ex = glm::vec3(M * glm::vec4(0.5f, 0, -0.5f, 1)) - o;
        const glm::vec3 ez = glm::vec3(M * glm::vec4(-0.5f, 0, 0.5f, 1)) - o;
        const glm::vec3 nrm = glm::normalize(glm::cross(ez, ex));
        // Park the camera on the page's own front side, off-axis so this is not the
        // trivial perpendicular case.
        const glm::vec3 pc = glm::vec3(M[3]);
        const glm::vec3 eye = pc + nrm * 2.0f + glm::normalize(ex) * 0.6f;
        glm::vec3 up(0, 1, 0);
        if (std::abs(glm::dot(glm::normalize(pc - eye), up)) > 0.9f) up = {1, 0, 0};
        const Camera cam = MakeCam(eye, pc, up);
        for (const glm::vec2 pn : {glm::vec2(0.5f, 0.5f), glm::vec2(0.44f, 0.57f),
                                   glm::vec2(0.56f, 0.46f)}) {
            const Hit h = Pick(s, cam, pn, {}, {}, P, nullptr);
            if (h.kind != Hit::Kind::Page) {
                Fail("4: sheared page missed at pointer " + std::to_string(pn.x));
                continue;
            }
            glm::vec3 ro(0.0f), rd(0.0f);
            cam.ScreenRay(pn, ro, rd);
            const f32 den = glm::dot(rd, nrm);
            if (std::abs(den) < 1e-6f) continue;
            const f32 t = glm::dot(o - ro, nrm) / den;
            const glm::vec3 hp = ro + rd * t;
            // ex and ez are NOT orthogonal under shear - that is the whole point of
            // this case - so solve hp-o = u*ex + v*ez properly (Gram 2x2) rather
            // than projecting onto each axis independently.
            const glm::vec3 rel = hp - o;
            const f32 aa = glm::dot(ex, ex), ab = glm::dot(ex, ez), bb = glm::dot(ez, ez);
            const f32 d1 = glm::dot(rel, ex), d2 = glm::dot(rel, ez);
            const f32 det2 = aa * bb - ab * ab;
            if (std::abs(det2) < 1e-12f) continue;
            const f32 u = (d1 * bb - d2 * ab) / det2;
            const f32 v = (aa * d2 - ab * d1) / det2;
            const glm::vec2 want(u * 800.0f, v * 600.0f);
            if (glm::distance(want, h.canvasPx) > 0.5f) {
                Fail("4: sheared/rotated page maps to the wrong canvas pixel (got " +
                     std::to_string(h.canvasPx.x) + "," + std::to_string(h.canvasPx.y) +
                     " want " + std::to_string(want.x) + "," + std::to_string(want.y) + ")");
            }
        }
    }

    // 5. BACK FACE IS INERT, and a MIRRORED page's VISIBLE face is the live one.
    {
        Scene s;
        MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera below = MakeCam({0, -5, 0}, {0, 0, 0}, kTopDownUp);
        Check(Pick(s, below, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "5: the BACK of a page is clickable");

        // One negative scale axis flips HANDEDNESS: the quad's triangles wind the
        // other way, so its geometric front face is now -Y. The old
        // `M * (0,1,0,0)` normal still said +Y and made the face you cannot see the
        // interactive one. The interactive face must follow the GEOMETRY, so a
        // mirrored page is live from below and inert from above - the exact
        // opposite of the un-mirrored case above, which is the whole point.
        Scene s2;
        MakePage(s2, {0, 0, 0}, {0, 0, 0}, {-1.0f, 1.0f, 1.0f}, 2.0f, 512, 512);
        const Camera above = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        Check(Pick(s2, above, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "5b: a MIRRORED page's BACK (the face pointing away) is clickable");
        Check(Pick(s2, below, centre, {}, {}, P, nullptr).kind == Hit::Kind::Page,
              "5c: a MIRRORED page's geometric front face is inert");
    }

    // 6. OUT-OF-RANGE POINTER. Only the negative sentinel used to be guarded, so a
    //    pointer past 1.0 (the cursor outside the editor's letterboxed Game image)
    //    unprojected happily and picked pages off-screen.
    {
        Scene s;
        MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 40.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        for (const glm::vec2 pn : {glm::vec2(1.5f, 0.5f), glm::vec2(0.5f, 1.5f),
                                   glm::vec2(-0.2f, 0.5f), glm::vec2(0.5f, -0.2f)}) {
            ui::PointerState ps;
            ui::ComputeWorldPointers(s, cam, pn, ps);
            Check(ps.worldCanvasPx.empty(), "6: an off-screen pointer picked a page");
            Check(Pick(s, cam, pn, {}, {}, P, nullptr).kind == Hit::Kind::None,
                  "6: an off-screen pointer produced a hit");
        }
    }

    // 7. A PAGE MOVING UNDER A PARENT keeps picking correctly - the surfaceInv
    //    cache must invalidate. Drive the parent through several poses with ONE
    //    shared UIContext (a static page skips the inverse; a moving one must not).
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity mount = s.CreateEntity("mount");
        reg.emplace_or_replace<Transform>(mount, Transform{});
        const entt::entity c =
            MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        reg.emplace<Parent>(c, Parent{mount});
        ui::UIContext ctx;
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        for (int i = 0; i < 6; ++i) {
            const f32 dx = static_cast<f32>(i) * 0.15f;
            reg.get<Transform>(mount).position = {dx, 0.0f, 0.0f};
            const Hit h = Pick(s, cam, centre, {}, {}, P, &ctx);
            // The page slid +dx, so the centre ray lands dx to the LEFT of centre
            // in canvas space: u = 0.5 - dx/worldWidth.
            const f32 wantU = 0.5f - dx / 2.0f;
            if (wantU < 0.0f) {
                Check(h.kind == Hit::Kind::None, "7: page slid away but still picked");
                continue;
            }
            if (h.kind != Hit::Kind::Page) {
                Fail("7: moving page lost the pointer at dx=" + std::to_string(dx));
                continue;
            }
            Check(std::abs(h.canvasPx.x - wantU * 512.0f) < 0.5f,
                  "7: STALE surfaceInv - a moving page picks at its old position (dx=" +
                      std::to_string(dx) + ", got " + std::to_string(h.canvasPx.x) + ")");
        }
        // Then destroy the page and confirm the cache does not answer for a ghost.
        reg.destroy(reg.get<UICanvas>(c).surface);
        reg.destroy(c);
        Check(Pick(s, cam, centre, {}, {}, P, &ctx).kind == Hit::Kind::None,
              "7b: a destroyed page still picks out of the cache");
    }

    // 8. UNIFICATION: an Interactable and a page under the same reticle produce
    //    exactly ONE winner, and it is the nearer. Then occlusion for objects, and
    //    the proximity fallback when the player is not aiming at anything.
    {
        Scene s;
        PhysicsWorld phys;
        entt::registry& reg = s.Registry();
        Params PO = P;
        PO.considerObjects = true;
        PO.hasAnchor = true;
        PO.anchor = {0, 5, 0};

        // Object at y=2 (3 m from the camera at y=5), page at y=0 (5 m).
        const entt::entity obj = s.CreateEntity("terminal");
        Transform ot;
        ot.position = {0, 2, 0};
        reg.emplace_or_replace<Transform>(obj, ot);
        reg.emplace<AABB>(obj, AABB{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}});
        Interactable ia;
        ia.range = 10.0f;
        reg.emplace<Interactable>(obj, ia);
        const entt::entity page =
            MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);

        Hit h = Pick(s, cam, centre, {}, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::Object && h.entity == obj,
              "8: the nearer OBJECT did not win over the page");
        Check(h.aimed, "8: the aim-first branch did not claim it");

        // Move the object behind the page: now the page must win.
        reg.get<Transform>(obj).position = {0, -2, 0};
        h = Pick(s, cam, centre, {}, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::Page && h.entity == page,
              "8b: the nearer PAGE did not win over the object");

        // Object occlusion: put a wall between the camera and the object.
        reg.get<Transform>(obj).position = {0, 2, 0};
        reg.get<UICanvas>(page).visible = false;
        MakeStaticBox(s, {0, 3.5f, 0}, {2.0f, 0.25f, 2.0f});
        phys.Update(s, 1.0f / 60.0f);
        const OccludeFn occ = PhysicsOccluder(phys);
        h = Pick(s, cam, centre, occ, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::None,
              "8c: an object BEHIND A WALL is still offered (aimed)");

        // Not aiming at it (look away) -> the proximity fallback finds it, but the
        // wall still hides it; remove the wall and it comes back.
        const Camera away = MakeCam({0, 5, 0}, {0, 5, -1});
        h = Pick(s, away, centre, occ, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::None,
              "8d: the proximity fallback still prompts through a wall");
        std::vector<entt::entity> walls;
        for (const entt::entity e : reg.view<RigidBody>()) walls.push_back(e);
        for (const entt::entity e : walls) reg.destroy(e);
        phys.Update(s, 1.0f / 60.0f);
        h = Pick(s, away, centre, occ, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::Object && h.entity == obj && !h.aimed,
              "8e: the proximity fallback lost an unobstructed object");

        // The gate is still range: push it out of range and nothing is offered.
        reg.get<Interactable>(obj).range = 0.5f;
        h = Pick(s, away, centre, occ, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::None, "8f: the range GATE stopped gating");
    }

    // 9. PROXIMITY GATE MEASURES TO THE BOX, not to the centre: standing against a
    //    long object whose CENTRE is far away must still offer it.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity e = s.CreateEntity("longbar");
        reg.emplace_or_replace<Transform>(e, Transform{});
        reg.emplace<AABB>(e, AABB{{-10.0f, -0.5f, -0.5f}, {10.0f, 0.5f, 0.5f}});
        Interactable ia;
        ia.range = 2.0f;
        reg.emplace<Interactable>(e, ia);
        Check(PointEntityBoxDistance(s, e, {9.5f, 0, 0}, 0.1f) < 1e-3f,
              "9: a point inside the box is not at distance 0");
        Params PO = P;
        PO.considerObjects = true;
        PO.hasAnchor = true;
        PO.anchor = {9.5f, 0.0f, 3.0f}; // 2.5 m from the bar's side, centre is 9.5 m away
        const Camera cam = MakeCam({9.5f, 0.0f, 3.0f}, {9.5f, 0.0f, 0.0f});
        Check(Pick(s, cam, centre, {}, {}, PO, nullptr).kind == Hit::Kind::None,
              "9b: out of range by box distance but still offered");
        PO.anchor = {9.5f, 0.0f, 2.0f};
        const Camera cam2 = MakeCam({9.5f, 0.0f, 2.0f}, {9.5f, 0.0f, 0.0f});
        Check(Pick(s, cam2, centre, {}, {}, PO, nullptr).kind == Hit::Kind::Object,
              "9c: in range by box distance but not offered (centre-distance gate)");
    }

    // 10. UICanvas::interactRange caps a page's own pick distance.
    {
        Scene s;
        const entt::entity c = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::Page,
              "10: default interactRange (0 = unlimited) is not unlimited");
        s.Registry().get<UICanvas>(c).interactRange = 3.0f;
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "10b: interactRange did not cap the pick distance");
    }

    // 11. AN INVISIBLE PAGE IS INERT (visible is authored and animator-driven).
    {
        Scene s;
        const entt::entity c = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        s.Registry().get<UICanvas>(c).visible = false;
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "11: a hidden page is still clickable");
    }

    // 12. SCREEN-SPACE Z-ORDER: exactly ONE element receives hover/press, and it is
    //     the TOPMOST. Screen-space hit-testing had no z-order at all - every
    //     overlapping element under the pointer hovered and clicked - which is
    //     incoherent next to a world pick that returns one winner. Draw order is
    //     layout order, so topmost = the later element.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity a = MakeButton(s, {0, 0}, {200, 100});
        const entt::entity b = MakeButton(s, {0, 0}, {200, 100}); // exactly the same rect
        ui::CanvasConfig cfg;
        // Ask the LAYOUT which one is on top rather than assuming a creation-order
        // convention: LayoutUI emits in draw order, so the LAST item is drawn last
        // and is therefore the topmost. (For canvas-less roots that is the reverse
        // of creation order, because an entt view iterates newest-first - exactly
        // the kind of thing a hand-written expectation would get wrong.)
        std::vector<ui::LayoutItem> layout;
        ui::LayoutUI(s, {1920, 1080}, cfg, layout);
        entt::entity top = entt::null, under = entt::null;
        for (const ui::LayoutItem& it : layout)
            if (it.entity == a || it.entity == b) top = it.entity;
        under = (top == a) ? b : a;

        Input in;
        Press(in, true);
        ui::PointerState ps;
        ui::UpdateInteraction(s, in, centre, {1920, 1080}, cfg, &ps);
        Check(reg.get<UIElement>(top).clicked && reg.get<UIElement>(top).hovered,
              "12: the TOPMOST overlapping button did not get the click");
        Check(!reg.get<UIElement>(under).clicked && !reg.get<UIElement>(under).hovered,
              "12: an OVERLAPPED button under the topmost one also clicked");
    }

    // 13. SCREEN SPACE BEATS WORLD SPACE. A menu overlay is by definition in front
    //     of the 3D world; a click on it must not also press a world page directly
    //     behind the cursor.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity screenBtn = MakeButton(s, {0, 0}, {200, 100});
        const entt::entity canvas =
            MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const entt::entity pageBtn = MakeButton(s, {0, 0}, {200, 100}, canvas);
        Input in;
        Press(in, true);
        ui::CanvasConfig cfg;
        ui::PointerState ps;
        ps.worldCanvasPx[static_cast<u32>(canvas)] = {256.0f, 256.0f};
        ui::UpdateInteraction(s, in, centre, {1920, 1080}, cfg, &ps);
        Check(reg.get<UIElement>(screenBtn).clicked,
              "13: the screen-space button did not get the click");
        Check(!reg.get<UIElement>(pageBtn).clicked,
              "13: the click went THROUGH the screen overlay into a world page");
    }

    // 14. RETICLE MODE: with the cursor locked, a world page's Button is pressed by
    //     the Interact ACTION, not by the left mouse button (LMB is fire).
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity canvas =
            MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const entt::entity btn = MakeButton(s, {0, 0}, {200, 100}, canvas);
        ui::CanvasConfig cfg;

        // LMB down, Interact NOT pressed -> nothing.
        {
            Input in;
            Press(in, true);
            ui::PointerState ps;
            ps.worldCanvasPx[static_cast<u32>(canvas)] = {256.0f, 256.0f};
            ps.worldButtonOverride = true;
            ps.worldPressed = false;
            ps.worldDown = false;
            ui::UpdateInteraction(s, in, {-1.0f, -1.0f}, {1920, 1080}, cfg, &ps);
            Check(!reg.get<UIElement>(btn).clicked,
                  "14: LMB pressed a world button while the cursor was locked");
            Check(reg.get<UIElement>(btn).hovered,
                  "14: the reticle did not hover the world button");
        }
        // Interact pressed -> clicked.
        {
            Input in;
            ui::PointerState ps;
            ps.worldCanvasPx[static_cast<u32>(canvas)] = {256.0f, 256.0f};
            ps.worldButtonOverride = true;
            ps.worldPressed = true;
            ps.worldDown = true;
            ui::UpdateInteraction(s, in, {-1.0f, -1.0f}, {1920, 1080}, cfg, &ps);
            Check(reg.get<UIElement>(btn).clicked,
                  "14b: the Interact action did not press the reticle-aimed world button");
        }
    }

    // 15. THE WHEEL STILL BUBBLES. Scroll lists are made of buttons; if the wheel
    //     followed the single hover winner, a list would stop scrolling wherever a
    //     Button happened to sit under the cursor.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        // The Button must end up TOPMOST (canvas-less roots lay out newest-first,
        // so the first-created root is drawn last).
        const entt::entity btn = MakeButton(s, {0, 0}, {200, 100});
        const entt::entity view = s.CreateEntity("scroll");
        {
            UIElement el;
            el.type = UIElement::Type::ScrollView;
            el.size = {400.0f, 200.0f};
            el.scrollVertical = true;
            el.scrollSpeed = 40.0f;
            reg.emplace<UIElement>(view, std::move(el));
        }
        const entt::entity content = s.CreateEntity("content");
        {
            UIElement el;
            el.type = UIElement::Type::Label;
            el.size = {380.0f, 900.0f}; // taller than the view -> scrollable
            reg.emplace<UIElement>(content, std::move(el));
            reg.emplace<Parent>(content, Parent{view});
        }
        Input in;
        in.NewFrame();
        in.OnMouseWheel(-1.0f); // one notch toward the user
        ui::CanvasConfig cfg;
        ui::PointerState ps;
        ui::UpdateInteraction(s, in, centre, {1920, 1080}, cfg, &ps);
        Check(reg.get<UIElement>(view).scrollPos.y > 0.0f,
              "15: the wheel stopped scrolling a list because a Button was on top");
        Check(reg.get<UIElement>(btn).hovered && !reg.get<UIElement>(view).hovered,
              "15b: the wheel target also stole the hover (two hovered elements)");
    }

    // 16. TIE-BREAK IS AUTHORED, NOT POOL ORDER. Two COPLANAR pages (an overlay
    //     bolted to the same wall plane as its base page) hit at the same `t`.
    //     "Whichever the view yielded first" made the winner depend on creation
    //     order, which flips on a document reopen, an undo or a shard respawn.
    {
        const auto build = [&](bool overlayFirst) {
            Scene s;
            entt::registry& reg = s.Registry();
            entt::entity base = entt::null, overlay = entt::null;
            const auto mkOverlay = [&] {
                overlay = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 256, 256);
                reg.get<UICanvas>(overlay).sortOrder = 10;
            };
            const auto mkBase = [&] {
                base = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
                reg.get<UICanvas>(base).sortOrder = 0;
            };
            if (overlayFirst) { mkOverlay(); mkBase(); } else { mkBase(); mkOverlay(); }
            const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
            const Hit h = Pick(s, cam, centre, {}, {}, P, nullptr);
            return std::make_pair(h.entity == overlay, h.kind == Hit::Kind::Page);
        };
        const auto a = build(true);
        const auto b = build(false);
        Check(a.second && b.second, "16: coplanar pages produced no hit at all");
        Check(a.first && b.first,
              "16: a coplanar TIE did not go to the higher sortOrder (it went to "
              "whatever entt yielded first)");
    }

    // 17. A PAGE IS NOT OCCLUDED BY THE PROP IT IS MOUNTED ON. The wall terminal /
    //     tablet / recessed-screen case: the housing's collider front face is
    //     always ahead of the screen behind it, so without a self-exemption the
    //     page is permanently, silently unclickable.
    {
        Scene s;
        PhysicsWorld phys;
        entt::registry& reg = s.Registry();
        // A prop with a Box collider; the page is its CHILD, recessed inside it.
        const entt::entity prop = MakeStaticBox(s, {0, 0, 0}, {1.5f, 0.5f, 1.5f});
        const entt::entity page =
            MakePage(s, {0, -0.2f, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        reg.emplace<Parent>(page, Parent{prop});
        phys.Update(s, 1.0f / 60.0f);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        const OccludeFn occ = PhysicsOccluder(phys);
        const Hit h = Pick(s, cam, centre, occ, {}, P, nullptr);
        Check(h.kind == Hit::Kind::Page && h.entity == page,
              "17: a page recessed into its OWN housing is occluded by it");
        // ...but a DIFFERENT body in front still occludes it.
        MakeStaticBox(s, {0, 3.0f, 0}, {2.0f, 0.25f, 2.0f});
        phys.Update(s, 1.0f / 60.0f);
        Check(Pick(s, cam, centre, occ, {}, P, nullptr).kind == Hit::Kind::None,
              "17b: the self-exemption also let a real wall through");
    }

    // 18. A CHARACTER BLOCKS THE RAY. CharacterControllers own no physics body
    //     (CharacterVirtual is not in the broadphase), so the occlusion cast was
    //     blind to every NPC: the reticle picked a wall page through their chest.
    //     The player's OWN capsule must still not block (third-person camera).
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity page =
            MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::Page,
              "18: baseline - the page is not pickable with nothing in the way");

        const entt::entity npc = s.CreateEntity("npc");
        Transform nt;
        nt.position = {0, 2.0f, 0};
        reg.emplace_or_replace<Transform>(npc, nt);
        CharacterController cc;
        reg.emplace<CharacterController>(npc, cc);
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "18b: the pick went THROUGH an NPC's capsule into the page behind it");

        // Exempted (this is the player) -> the page is reachable again.
        Params PA = P;
        PA.anchorEntity = npc;
        Check(Pick(s, cam, centre, {}, {}, PA, nullptr).kind == Hit::Kind::Page,
              "18c: the player's own capsule occludes the player's own ray");
        // ...and the opt-out works.
        Params PN = P;
        PN.charactersOcclude = false;
        Check(Pick(s, cam, centre, {}, {}, PN, nullptr).kind == Hit::Kind::Page,
              "18d: charactersOcclude=false did not opt out");
        (void)page;
    }

    // 19. A DISTANT PAGE DOES NOT SILENCE A NEARBY PROMPT. The regression the
    //     unified pass introduced: page candidacy has no interactivity test and no
    //     default range limit, and the aimed page used to `return` before the
    //     proximity fallback ever ran - so a decorative diegetic screen far down
    //     the corridor removed "[E] Talk" from an NPC one metre away.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        // The page is 30 m below the camera; the object sits BESIDE the player,
        // out of the ray entirely, so only the fallback can find it.
        MakePage(s, {0, -30.0f, 0}, {0, 0, 0}, {1, 1, 1}, 20.0f, 512, 512);
        const entt::entity npc = s.CreateEntity("npc");
        Transform nt;
        nt.position = {1.0f, 0.0f, 0.0f};
        reg.emplace_or_replace<Transform>(npc, nt);
        reg.emplace<AABB>(npc, AABB{{-0.3f, -0.9f, -0.3f}, {0.3f, 0.9f, 0.3f}});
        Interactable ia;
        ia.range = 2.5f;
        reg.emplace<Interactable>(npc, ia);
        Params PO = P;
        PO.considerObjects = true;
        PO.hasAnchor = true;
        PO.anchor = {0, 0, 0};
        const Camera cam = MakeCam({0, 0, 0}, {0, -1, 0}, kTopDownUp);
        const Hit h = Pick(s, cam, centre, {}, {}, PO, nullptr);
        Check(h.kind == Hit::Kind::Object && h.entity == npc && !h.aimed,
              "19: a distant page suppressed the proximity prompt of a nearby object");
        Check(h.canvasPx == glm::vec2(0.0f),
              "19b: the object took the pick but left the page's canvas pixels behind");

        // ...and the page still wins when it is the NEARER of the two: aim only
        // loses to proximity when the proximity candidate is genuinely closer.
        reg.get<Transform>(npc).position = {45.0f, 0.0f, 0.0f};
        reg.get<Interactable>(npc).range = 60.0f;
        Check(Pick(s, cam, centre, {}, {}, PO, nullptr).kind == Hit::Kind::Page,
              "19c: the nearer PAGE lost to a farther proximity object");
    }

    // 20. A PAGE UNDER A CLOSED SCREEN IS INERT. Each screen is its own document
    //     now, so a diegetic 3D button naturally gets authored under that screen's
    //     UIPanel root - and the canvas walk starts AT the canvas, so nothing
    //     above it was ever gated: a hidden screen's world button stayed clickable.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity screen = s.CreateEntity("Settings");
        UIPanel panel;
        panel.name = "Settings";
        panel.active = true;
        reg.emplace<UIPanel>(screen, panel);
        const entt::entity pc = MakePage(s, {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 2.0f, 512, 512);
        reg.emplace<Parent>(pc, Parent{screen});
        const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::Page,
              "20: a page under an ACTIVE screen is not pickable");
        reg.get<UIPanel>(screen).active = false;
        Check(Pick(s, cam, centre, {}, {}, P, nullptr).kind == Hit::Kind::None,
              "20b: a page under a CLOSED screen is still clickable");
    }

    // 21. THE POINTER POLICY's screen-focus guard applies to BOTH reticle reasons.
    //     It read `padActive && !screenFocusActive`, so a LOCKED cursor - which is
    //     exactly where a HUD focus ring lives - re-enabled the reticle and one
    //     press fired the focused HUD widget AND the world page behind it.
    {
        PointerInputs in;
        in.cursorLocked = true;
        in.padActive = true;
        in.screenFocusActive = true;
        const PointerMode m = ResolvePointer(in);
        Check(!m.reticle && !m.useInteractAction,
              "21: a live SCREEN focus ring did not suspend the locked-cursor reticle");
        in.screenFocusActive = false;
        Check(ResolvePointer(in).reticle, "21b: the reticle stopped working entirely");
    }

    // 22. Interactable::range is honoured WITHOUT an anchor (measured along the
    //     ray). It was ignored outright, so every Interactable within maxRange
    //     (100 m) under the reticle was a candidate for an anchor-less caller.
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity e = s.CreateEntity("far");
        Transform t;
        t.position = {0, -20.0f, 0};
        reg.emplace_or_replace<Transform>(e, t);
        reg.emplace<AABB>(e, AABB{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}});
        Interactable ia;
        ia.range = 2.5f;
        reg.emplace<Interactable>(e, ia);
        Params PO = P;
        PO.considerObjects = true; // hasAnchor stays FALSE
        const Camera cam = MakeCam({0, 0, 0}, {0, -1, 0}, kTopDownUp);
        Check(Pick(s, cam, centre, {}, {}, PO, nullptr).kind == Hit::Kind::None,
              "22: an anchor-less pick ignored Interactable::range entirely");
        reg.get<Interactable>(e).range = 50.0f;
        Check(Pick(s, cam, centre, {}, {}, PO, nullptr).kind == Hit::Kind::Object,
              "22b: an in-range object was rejected by the ray-distance gate");
    }

    // 23. COST. The pass runs once per pointer per frame and the whole engine has
    //     ~0.4 ms of CPU headroom, so the number matters as much as the behaviour.
    //     Measured here rather than asserted: a threshold would fail on a slower
    //     machine, but a printed number is checkable against the budget.
    {
        // The OBJECT half is measured too. The table used to create pages and
        // static boxes and ZERO Interactables, so `view<Interactable>()` was empty
        // in every row: the aimed OBB loop, the proximity fallback (a full scan
        // with a glm::inverse each) and its up-to-8 extra casts were all unmeasured
        // and the "1.00 cast/frame" figure only held for a scene with nothing
        // interactive in it. The last two rows are the real worst case.
        struct Row { const char* label; int pages; bool physics; int objs; bool inRange; };
        const Row rows[] = {
            {"0 pages,  0 obj, no cast   ", 0, false, 0, false},
            {"0 pages,  0 obj, + cast    ", 0, true, 0, false},
            {"1 page,   0 obj, + cast    ", 1, true, 0, false},
            {"4 pages,  0 obj, + cast    ", 4, true, 0, false},
            {"16 pages, 0 obj, + cast    ", 16, true, 0, false},
            {"1 page,  16 obj out of rng ", 1, true, 16, false},
            {"1 page,  16 obj IN range   ", 1, true, 16, true},
        };
        std::printf("  cost (per pick, RelWithDebInfo, one core):\n");
        for (const Row& r : rows) {
            Scene s;
            PhysicsWorld phys;
            for (int i = 0; i < r.pages; ++i)
                MakePage(s, {0.0f, -static_cast<f32>(i) * 0.1f, 0.0f}, {0, 0, 0}, {1, 1, 1},
                         2.0f, 512, 512);
            // Interactables OFF the ray (so the aimed loop rejects them and the
            // fallback has to do the work), either inside or outside `range`.
            for (int i = 0; i < r.objs; ++i) {
                const entt::entity e = s.CreateEntity("ia");
                Transform t;
                t.position = {2.0f + static_cast<f32>(i) * 0.25f, 5.0f, 0.0f};
                s.Registry().emplace_or_replace<Transform>(e, t);
                s.Registry().emplace<AABB>(e, AABB{{-0.4f, -0.9f, -0.4f}, {0.4f, 0.9f, 0.4f}});
                Interactable ia;
                ia.range = r.inRange ? 40.0f : 0.25f;
                s.Registry().emplace<Interactable>(e, ia);
            }
            // A representative world: 64 static bodies for the ray to sort through.
            for (int i = 0; i < 64; ++i)
                MakeStaticBox(s, {static_cast<f32>(i % 8) * 3.0f - 12.0f, -3.0f,
                                  static_cast<f32>(i / 8) * 3.0f - 12.0f},
                              {1.0f, 1.0f, 1.0f});
            phys.Update(s, 1.0f / 60.0f);
            const Camera cam = MakeCam({0, 5, 0}, {0, 0, 0}, kTopDownUp);
            const OccludeFn occ = r.physics ? PhysicsOccluder(phys) : OccludeFn{};
            ui::UIContext ctx;
            Params PC;
            PC.considerObjects = true;
            PC.hasAnchor = true;
            PC.anchor = {0, 5, 0};
            constexpr int kIters = 20000;
            volatile int sink = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kIters; ++i)
                sink += static_cast<int>(Pick(s, cam, centre, occ, {}, PC, &ctx).kind);
            const f64 us =
                std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count() /
                kIters;
            (void)sink;
            std::printf("    %s : %7.3f us  (%.4f ms)\n", r.label, us, us / 1000.0);
        }
    }

    if (g_fails == 0)
        HBE_INFO("PickTest: pages occlude, nearest wins, sheared/mirrored pages map "
                 "correctly, moving pages invalidate, objects and pages share one winner.");
    return g_fails == 0;
}

} // namespace hbe::interact
