// Interaction/Pick.h - THE picking pass. One ray, one occlusion cast, one winner.
//
// Before this file there were two independent claimants on a single player input:
//
//   * world-space UI pages - an analytic ray/plane test with NO occlusion and NO
//     nearest-surface resolution, which wrote EVERY page it passed through (so a
//     button behind a wall was clickable, and two stacked pages both took the
//     click); and
//   * Interactable objects - a pure RADIUS test with no ray at all, no occlusion
//     and no facing (so an NPC through a wall prompted, and so did one behind you).
//
// They only avoided colliding because each was suppressed while the other could
// run: `[E]` interaction and world-UI clicking were mutually exclusive BY
// CONSTRUCTION. That is exactly the wall you hit the moment you want a 3D button
// you press while playing.
//
// interact::Pick unifies them. It casts ONE ray, does ONE physics query for the
// occlusion horizon, and returns the single nearest interactive thing - a page or
// an object - so the affordance channel (one reticle, one prompt, one hover) can
// never have two winners that presentation has to tie-break.
//
// MULTIPLE CAMERAS: the camera is a PARAMETER, not `renderer.GetCamera()`, and the
// pass keeps no state of its own beyond the caller's ui::UIContext. Split-screen or
// a portal view is therefore one call per view with that view's camera and pointer
// - but the ENGINE still drives exactly one, because the rest of the frame (the
// reticle, the prompt anchor, the free-cursor policy) is single-view.
//
// THREADING: main thread only. PhysicsWorld raycasts are documented main-thread-
// only (terrain SetHeights races concurrent queries), and this is called
// synchronously from the frame loop.
//
// WHAT OCCLUDES: only entities with a RigidBody, plus terrain. A MeshInstance
// with no collider blocks nothing. The content rule is "if it should block
// interaction, give it a collider" - pages deliberately stay collider-less so they
// never block bullets or character sweeps.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>

namespace hbe {

class Scene;
class Camera;
namespace ui { struct UIContext; }

namespace interact {

// What the world put in the way. Bind PhysicsWorld::RaycastDetailed to this; an
// EMPTY function means "nothing occludes", which is what a headless caller with
// no physics world gets (and what ui::ComputeWorldPointers has always done).
struct Block {
    bool hit = false;
    f32 distance = 0.0f;
    entt::entity entity = entt::null;
};
using OccludeFn = std::function<Block(const glm::vec3& origin, const glm::vec3& dir,
                                      f32 maxDist)>;

// "Is this Interactable a legal candidate right now?" The once/fired, requiredFlag
// and already-picked-up gating stays in the Engine, where the story flags live.
using AcceptFn = std::function<bool(entt::entity)>;

struct Params {
    f32 maxRange = 100.0f; // ray length considered at all
    // Occlusion tolerance. A page mounted flush on a wall shares that wall's
    // collider surface; without a skin it would lose to the wall it is bolted to.
    // AUTHORING RULE: mount world pages >= 2 cm proud of their backing surface, or
    // clear UICanvas::occlude.
    f32 skin = 0.02f;
    bool considerObjects = true; // false = pages only (world-UI-only callers)
    // The player, for the Interactable PROXIMITY GATE and the not-aiming fallback.
    // Without it, objects are aim-only and Interactable::range is measured along
    // the RAY instead (so `range` still gates a caller that has no player).
    bool hasAnchor = false;
    glm::vec3 anchor{0.0f};
    // The player's ENTITY, when it has one. CharacterControllers are occluders
    // (see below) and the one you are looking out of must not occlude you: in
    // third person the camera sits behind the player's own capsule, and without
    // this exemption the player would block every interaction in the game.
    entt::entity anchorEntity = entt::null;
    // Do CharacterControllers block the ray? They carry NO physics body
    // (PhysicsWorld builds a JPH::CharacterVirtual, which NarrowPhaseQuery cannot
    // see), so the occlusion cast is blind to every NPC - you could click a wall
    // page through an NPC's chest. This adds an explicit capsule test for them.
    bool charactersOcclude = true;
    // Most casts the proximity fallback may spend walking outward through occluded
    // candidates. Aim-first hits cost none of these.
    u32 maxFallbackCasts = 8;
};

struct Hit {
    enum class Kind : u8 { None = 0, Page = 1, Object = 2 };
    Kind kind = Kind::None;
    entt::entity entity = entt::null; // the UICanvas (Page) or the Interactable (Object)
    f32 distance = 0.0f;              // along the ray when `aimed`, else from `anchor`
    glm::vec2 canvasPx{0.0f};         // Page only: the hit in that canvas's pixel space
    glm::vec3 point{0.0f};            // world-space hit point (Page only)
    bool aimed = false;               // true = the ray chose it; false = proximity fallback

    // The ray this pass used, and what the occlusion cast found. Kept so the
    // caller can reuse the horizon instead of casting again.
    bool hasRay = false;
    glm::vec3 origin{0.0f}, dir{0.0f};
    f32 wallDistance = 0.0f;
    entt::entity wallEntity = entt::null;

    // Cost accounting (what --benchmark / the cost report reads).
    u32 pagesTested = 0;
    u32 objectsTested = 0;
    u32 raycasts = 0;
};

// The pass. `pointerNorm` is 0..1 y-down (screen centre {0.5,0.5} is the reticle
// in cursor-locked gameplay); anything outside 0..1 means "no pointer" and the
// result is Kind::None with hasRay false.
Hit Pick(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
         const OccludeFn& occlude, const AcceptFn& accept, const Params& params,
         ui::UIContext* ctx);

// --- The pointer policy ------------------------------------------------------
// WHERE THE WORLD POINTER COMES FROM, and what presses it. One pure function so
// the rule has exactly one definition: the Engine calls it every frame and the
// self-test calls the same code, instead of the test re-deriving a policy that
// could then drift from the shipped one.
struct PointerInputs {
    bool external = false;             // editor feeds the pointer (Game panel)
    glm::vec2 externalNorm{-1.0f};     // ...this value, normalized over that image
    bool cursorLocked = false;         // first-person: the OS cursor is captured
    bool padActive = false;            // the most recent input came from a gamepad
    // A focus ring is live on a SCREEN element (gamepad/keyboard navigation is
    // driving the menu). Screen space beats world space, so while this is true the
    // pad's reticle must not also be pressing a world page behind the menu.
    bool screenFocusActive = false;
    glm::vec2 cursorNorm{-1.0f};       // the OS cursor, normalized over the window
};
struct PointerMode {
    // Pointer for WORLD-space pages (a page's own canvas pixels come from the ray).
    glm::vec2 worldPointer{-1.0f};
    // Pointer for SCREEN-space canvases. Never the reticle: a crosshair must not
    // hover whatever HUD element happens to sit under it.
    glm::vec2 screenPointer{-1.0f};
    bool reticle = false; // worldPointer is screen centre rather than the cursor
    // World widgets are pressed with the Interact ACTION rather than LMB (LMB is
    // fire). True exactly when the reticle is driving them.
    bool useInteractAction = false;
};
PointerMode ResolvePointer(const PointerInputs& in);

// Ray vs the entity's world OBB (its local-space AABB under its world matrix).
// Entities with no AABB fall back to a sphere of `fallbackRadius` at the origin,
// so collider-less, bounds-less authored content stays reachable. `outT` is the
// entry distance (0 when the ray starts inside).
bool RayEntityBox(Scene& scene, entt::entity e, const glm::vec3& origin,
                  const glm::vec3& dir, f32 maxT, f32 fallbackRadius, f32& outT);

// Shortest distance from a world point to that same OBB (0 = inside). This is
// what the proximity GATE measures, so a large object no longer fails at contact
// the way a centre-to-pivot radius did.
f32 PointEntityBoxDistance(Scene& scene, entt::entity e, const glm::vec3& p,
                           f32 fallbackRadius);

// --test-uipick: the headless correctness gate for everything above. Pure CPU
// (Jolt only, no GPU, no window, no project). See Pick.cpp for the case list.
bool SelfTest();

// --test-3dinteract: the END-TO-END gate for 3D interactables - pointer source ->
// Pick -> ui::UpdateInteraction -> hover/held/clicked -> UIElement::action, plus the
// whole-object half. Covers all three input modes (free cursor, locked-cursor
// reticle, gamepad), occlusion by a wall AND by the terrain heightfield,
// nearest-wins in all three pairings, and STREAMED SHARD content (a real
// stream::Streamer against a real baked level). Lives in Interact3DTest.cpp so the
// pick pass itself never grows a Renderer / streaming dependency.
bool Interact3DSelfTest();

} // namespace interact
} // namespace hbe
