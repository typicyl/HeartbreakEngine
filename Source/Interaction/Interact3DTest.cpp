// Interaction/Interact3DTest.cpp - `--test-3dinteract`, the END-TO-END gate for
// 3D interactables.
//
// --test-uipick (Pick.cpp) proves the PICK PASS in isolation: geometry, occlusion,
// nearest-wins, the shear/mirror/off-screen guards. This file proves the thing the
// player actually touches, which is a longer chain than the pick:
//
//     pointer source (reticle / cursor / pad)
//        -> interact::Pick (one ray, one occlusion cast, one winner)
//        -> ui::PointerState -> ui::UpdateInteraction
//        -> UIElement hovered / held / clicked
//        -> UIElement::action, which is what schematics see
//
// and for the whole-object half, the same pick feeding the Interactable prompt and
// the Interact ACTION that fires it.
//
// WHAT A 3D BUTTON IS. There is no third concept. A 3D button is a
// UIElement::Type::Button on a WORLD-SPACE UICanvas, so it inherits skinning, state
// colours, 9-slice, sounds, animators, the .hbui editor and - the part that matters
// here - `UIElement::action`, which already routes to On UI Clicked / On UI Changed
// in schematics. Interactable stays the WHOLE-OBJECT affordance: one prompt, one
// verb, flag-gated, no pixel space. "If it has a face you read, it is a page; if it
// is a thing you use, it is an Interactable."
//
// THE THREE INPUT MODES, all covered below:
//   * FREE CURSOR   - the cursor is the pointer; LMB presses.
//   * LOCKED CURSOR - the RETICLE (screen centre) is the pointer; the Interact
//                     ACTION presses (LMB is fire).
//   * GAMEPAD       - the reticle again, in EVERY cursor state (a pad moves no
//                     mouse, and focus navigation deliberately never lands on a
//                     world page), pressed with Interact's pad binding.
//
// This file lives beside Pick.cpp rather than inside it because the streaming case
// needs Scene/TagStreaming + Renderer, and the pick pass itself must not grow those
// dependencies for the sake of a test.
#include "Interaction/Pick.h"

#include "Core/Input.h"
#include "Core/InputActions.h"
#include "Core/Log.h"
#include "Physics/PhysicsWorld.h"
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/TagShard.h"
#include "Scene/TagStreaming.h"
#include "Scene/TagTable.h"
#include "Scene/TerrainSystem.h"
#include "Scene/WorldState.h"
#include "UI/UISystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace hbe::interact {

namespace {

namespace fs = std::filesystem;

int g_fails = 0;
void Check(bool ok, const std::string& why) {
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL: %s\n", why.c_str());
    }
}

// ---------------------------------------------------------------------------
// World building
// ---------------------------------------------------------------------------

// A world-space page: the UICanvas plus the UISurface quad ui::UpdateWorldSurfaces
// would have generated for it (picking reads only the surface's Transform, so no
// renderer and no render target are needed to test it).
entt::entity MakePage(Scene& s, glm::vec3 pos, glm::quat rot, f32 worldWidth, f32 refW,
                      f32 refH) {
    entt::registry& reg = s.Registry();
    const entt::entity c = s.CreateEntity("page");
    Transform t;
    t.position = pos;
    t.rotation = rot;
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

// A full-page Button on a world canvas - the 3D button under test. `action` is the
// schematic hook: On UI Clicked fires on exactly this string.
entt::entity MakeButton(Scene& s, entt::entity canvas, const std::string& action) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity("btn3d");
    UIElement el;
    el.type = UIElement::Type::Button;
    el.action = action;
    el.text = "PRESS";
    el.anchorMin = {0.05f, 0.05f};
    el.anchorMax = {0.95f, 0.95f};
    reg.emplace<UIElement>(e, std::move(el));
    reg.emplace<Parent>(e, Parent{canvas});
    return e;
}

entt::entity MakeWall(Scene& s, glm::vec3 pos, glm::vec3 half) {
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

entt::entity MakeObject(Scene& s, glm::vec3 pos, glm::vec3 half, f32 range,
                        const char* prompt) {
    entt::registry& reg = s.Registry();
    const entt::entity e = s.CreateEntity(prompt);
    Transform t;
    t.position = pos;
    reg.emplace_or_replace<Transform>(e, t);
    reg.emplace<AABB>(e, AABB{-half, half});
    Interactable ia;
    ia.prompt = prompt;
    ia.range = range;
    ia.action = InteractAction::SetFlag;
    ia.flag = std::string("used.") + prompt;
    reg.emplace<Interactable>(e, ia);
    return e;
}

// A first-person camera: eye at `eye` looking down `fwd`, 16:9, 60 deg vertical.
constexpr f32 kNear = 0.05f;
Camera FpsCam(glm::vec3 eye, glm::vec3 fwd) {
    Camera c;
    c.SetPerspective(60.0f, 16.0f / 9.0f, kNear, 500.0f);
    c.LookAt(eye, eye + glm::normalize(fwd), {0.0f, 1.0f, 0.0f});
    return c;
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

// ---------------------------------------------------------------------------
// One engine frame, reproduced
// ---------------------------------------------------------------------------
// This mirrors Engine.cpp's UI-interaction block verbatim in ORDER and in the
// values it passes, so the test exercises the real composition rather than a
// re-derivation of it: pointer source -> Pick -> PointerState -> UpdateInteraction.
struct Rig {
    Scene* scene = nullptr;
    const Camera* cam = nullptr;
    OccludeFn occlude;
    input::ActionMap actions;
    bool hasAnchor = false;
    glm::vec3 anchor{0.0f};
    // A focus ring is live on a screen element (the pad is navigating a menu).
    bool screenFocusActive = false;

    // Result of the last frame.
    Hit pick;
    bool interactPressed = false; // the Interact edge this frame (object activation)

    // One frame. `cursorLocked` picks the pointer source; `lmb` is the left mouse
    // button; `interactKey` is the keyboard Interact key (E). `usePad` injects a
    // gamepad - headless has no XInput device, and both masks are explicit so the
    // caller owns the edge (press = (mask, 0), hold = (mask, mask)).
    //
    // Button state is set on EVERY frame, not only when true: keys and mouse
    // buttons latch until the platform sends an up event, so a missing release
    // would leave Interact held forever and every later edge assertion would be
    // testing nothing.
    void Frame(Input& in, bool cursorLocked, bool lmb, bool interactKey,
               bool usePad = false, u32 padMask = 0, u32 padPrev = 0) {
        in.NewFrame();
        in.OnMouseButton(MouseButton::Left, lmb);
        in.OnKeyVK(static_cast<u32>('E'), interactKey);
        if (usePad) in.InjectGamepadForTest(padMask, padPrev);

        // ---- the pointer: the SHIPPED policy, not a copy of it -----------------
        PointerInputs pin;
        pin.cursorLocked = cursorLocked;
        pin.padActive = in.LastInputWasGamepad();
        pin.screenFocusActive = screenFocusActive;
        // A free cursor parked at the screen centre; the reticle is the same point
        // by construction, which is what makes these two modes comparable here.
        pin.cursorNorm = {0.5f, 0.5f};
        const PointerMode pmode = ResolvePointer(pin);
        const glm::vec2 pointerNorm = pmode.worldPointer;
        const glm::vec2 screenPointer = pmode.screenPointer;

        // ---- one pick pass -----------------------------------------------------
        Params p;
        p.maxRange = 100.0f;
        p.considerObjects = true;
        p.hasAnchor = hasAnchor;
        p.anchor = anchor;
        pick = Pick(*scene, *cam, pointerNorm, occlude, {}, p, nullptr);

        ui::PointerState ps;
        if (pick.kind == Hit::Kind::Page)
            ps.worldCanvasPx[static_cast<u32>(pick.entity)] = pick.canvasPx;
        if (pmode.useInteractAction) {
            ps.worldButtonOverride = true;
            ps.worldPressed = actions.Pressed(in, "Interact");
            ps.worldDown = actions.Down(in, "Interact");
        }

        ui::CanvasConfig cfg;
        ui::UpdateInteraction(*scene, in, screenPointer, {1920.0f, 1080.0f}, cfg, &ps);

        // ---- the object half (Engine::UpdateInteractions reads the same pick) --
        interactPressed = actions.Pressed(in, "Interact");
    }

    // "Aim, then press once": a RELEASED frame followed by a pressed one, so the
    // press is a genuine down-EDGE. Needed because key/button state latches across
    // frames - a second Frame(..., interact=true) in a row is a HOLD, not a press,
    // and asserting `clicked` on it would silently be asserting nothing.
    void Tap(Input& in, bool cursorLocked) {
        Frame(in, cursorLocked, false, false);
        Frame(in, cursorLocked, false, true);
    }

    // The object the engine would prompt for this frame (its exact condition).
    entt::entity Object() const {
        if (pick.kind != Hit::Kind::Object) return entt::null;
        const entt::registry& reg = scene->Registry();
        if (!reg.valid(pick.entity) || !reg.all_of<Interactable>(pick.entity))
            return entt::null;
        return pick.entity;
    }
};

// The default keyboard-only Interact binding used by most cases below.
input::ActionMap KeyboardInteract() {
    input::ActionMap m;
    input::ActionDef d;
    d.name = "Interact";
    d.defaults.key = Key::E;
    m.SetDefinitions({d});
    return m;
}

const UIElement& El(Scene& s, entt::entity e) { return s.Registry().get<UIElement>(e); }

// ---------------------------------------------------------------------------
// Streaming: author a level whose shards carry the interactive content
// ---------------------------------------------------------------------------
entt::entity ByGuid(Scene& s, u64 guid) {
    const entt::registry& reg = s.Registry();
    for (const entt::entity e : reg.view<Guid>())
        if (reg.get<Guid>(e).value == guid) return e;
    return entt::null;
}

} // namespace

bool Interact3DSelfTest() {
    g_fails = 0;
    std::printf("--test-3dinteract: 3D interactables end to end\n");

    // ======================================================================
    // 1. FIRST-PERSON RETICLE: hover -> press -> hold -> release -> activate.
    //    The camera stands at the origin looking down -Z; the page is 3 m ahead,
    //    stood upright (its local +Y faces the camera), so the reticle is dead
    //    centre on a full-page Button.
    // ======================================================================
    // A page's local XZ plane faces its own +Y. Rotating +90 deg about X sends +Y
    // to +Z, i.e. the front face looks back down the camera's forward axis (-Z).
    const glm::quat kFacingCamera =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    {
        Scene s;
        const entt::entity canvas =
            MakePage(s, {0.0f, 0.0f, -3.0f}, kFacingCamera, 1.0f, 512.0f, 512.0f);
        const entt::entity btn = MakeButton(s, canvas, "kiosk:open");
        const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        Rig rig;
        rig.scene = &s;
        rig.cam = &cam;
        rig.actions = KeyboardInteract();
        Input in;

        // a) Aimed, nothing pressed -> HOVER only.
        rig.Frame(in, /*locked*/ true, /*lmb*/ false, /*interact*/ false);
        Check(rig.pick.kind == Hit::Kind::Page && rig.pick.entity == canvas,
              "1a: the reticle did not pick the 3D button's page");
        Check(El(s, btn).hovered, "1a: a reticle-aimed 3D button did not HOVER");
        Check(!El(s, btn).held, "1a: an untouched 3D button reported held");
        Check(!El(s, btn).clicked, "1a: an untouched 3D button reported clicked");

        // b) LMB is FIRE, not a UI press: it must not touch the button.
        rig.Frame(in, true, /*lmb*/ true, false);
        Check(!El(s, btn).clicked && !El(s, btn).held,
              "1b: the left mouse button pressed a 3D button (LMB is fire)");
        Check(El(s, btn).hovered, "1b: the hover was lost while LMB was down");

        // c) Interact DOWN-EDGE -> clicked AND held (press state visible on the
        //    very first frame, not a frame late).
        rig.Frame(in, true, false, /*interact*/ true);
        Check(El(s, btn).clicked, "1c: the Interact action did not activate the 3D button");
        Check(El(s, btn).held, "1c: the 3D button did not enter its PRESSED state");
        Check(El(s, btn).action == "kiosk:open",
              "1c: the activated button's action is not the authored one (schematics "
              "route On UI Clicked on exactly this string)");

        // d) Still held -> the press STATE persists while `clicked` (an edge) does
        //    not. This is the whole reason UIElement::held exists: a visual driven
        //    by `clicked` alone shows for one frame and a held button looks idle.
        //    (A plain Frame, deliberately NOT Tap: Tap releases first, which would
        //    manufacture a second edge and make this assertion vacuous.)
        rig.Frame(in, true, false, /*interact still down*/ true);
        Check(!El(s, btn).clicked, "1d: a held Interact re-fired the button every frame");
        Check(El(s, btn).held, "1d: the PRESSED state vanished while the button was held");

        // e) RELEASE -> back to hover.
        rig.Frame(in, true, false, false);
        Check(!El(s, btn).held, "1e: the 3D button stayed pressed after release");
        Check(El(s, btn).hovered, "1e: the 3D button lost its hover after release");

        // f) Look away -> nothing.
        const Camera away = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        rig.cam = &away;
        rig.Tap(in, true);
        Check(rig.pick.kind == Hit::Kind::None, "1f: a page BEHIND the camera was picked");
        Check(!El(s, btn).hovered && !El(s, btn).clicked && !El(s, btn).held,
              "1f: a 3D button behind the camera still took the press");
    }

    // ======================================================================
    // 2. FREE CURSOR drives the same button with LMB. Same page, same geometry -
    //    only the pointer source and the press verb differ.
    // ======================================================================
    {
        Scene s;
        const entt::entity canvas =
            MakePage(s, {0.0f, 0.0f, -3.0f}, kFacingCamera, 1.0f, 512.0f, 512.0f);
        const entt::entity btn = MakeButton(s, canvas, "kiosk:open");
        const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        Rig rig;
        rig.scene = &s;
        rig.cam = &cam;
        rig.actions = KeyboardInteract();
        Input in;

        rig.Frame(in, /*locked*/ false, false, false);
        Check(El(s, btn).hovered, "2: a free cursor over a 3D button did not hover it");
        rig.Frame(in, false, /*lmb*/ true, false);
        Check(El(s, btn).clicked && El(s, btn).held,
              "2: LMB did not press the 3D button with a FREE cursor");
    }

    // ======================================================================
    // 3. GAMEPAD. Two things have to be true, and the second is what makes it a
    //    real test: the pad ACTIVATES the button, and it does so THROUGH the
    //    Interact action - so a pad press with a keyboard-only binding must do
    //    nothing. And the pad must reach a world page with a FREE cursor too,
    //    because a pad moves no mouse and focus navigation deliberately never
    //    lands on a world canvas.
    // ======================================================================
    {
        Scene s;
        const entt::entity canvas =
            MakePage(s, {0.0f, 0.0f, -3.0f}, kFacingCamera, 1.0f, 512.0f, 512.0f);
        const entt::entity btn = MakeButton(s, canvas, "kiosk:open");
        const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        Rig rig;
        rig.scene = &s;
        rig.cam = &cam;
        Input in;

        // a) Keyboard-only binding: a pad press must NOT fire it.
        rig.actions = KeyboardInteract();
        rig.Frame(in, true, false, false, /*pad*/ true, Gamepad_Y, 0);
        Check(!El(s, btn).clicked,
              "3a: a gamepad button fired a 3D button that Interact is not bound to "
              "(the press is not going through the action map)");

        // b) Bind Interact to the pad. Press edge -> clicked + held.
        {
            input::ActionMap m;
            input::ActionDef d;
            d.name = "Interact";
            d.defaults.key = Key::E;
            d.defaults.pad = Gamepad_Y;
            m.SetDefinitions({d});
            rig.actions = m;
        }
        rig.Frame(in, true, false, false, /*pad*/ true, Gamepad_Y, 0);
        Check(El(s, btn).clicked, "3b: a gamepad press did not activate the 3D button");
        Check(El(s, btn).held, "3b: a gamepad press produced no PRESSED state");

        // c) Held (mask unchanged) -> held stays, clicked does not repeat.
        rig.Frame(in, true, false, false, /*pad*/ true, Gamepad_Y, Gamepad_Y);
        Check(!El(s, btn).clicked, "3c: a held pad button re-fired every frame");
        Check(El(s, btn).held, "3c: the PRESSED state dropped while the pad was held");

        // d) Released.
        rig.Frame(in, true, false, false, /*pad*/ true, 0, Gamepad_Y);
        Check(!El(s, btn).held, "3d: the 3D button stayed pressed after the pad release");

        // e) FREE CURSOR + gamepad: the reticle still drives the page. Before this,
        //    a pad user in a dialogue/pause overlay could not touch a world page at
        //    all - the pointer was the (motionless) mouse.
        rig.Frame(in, /*locked*/ false, false, false, /*pad*/ true, Gamepad_Y, 0);
        Check(rig.pick.kind == Hit::Kind::Page,
              "3e: a gamepad with a FREE cursor did not aim the reticle at the page");
        Check(El(s, btn).clicked,
              "3e: a gamepad could not press a world page while the cursor was free");
    }

    // ======================================================================
    // 4. A WALL BLOCKS IT. The occlusion horizon comes from the ONE physics cast
    //    the pick already makes; a page/object behind a solid collider is not a
    //    candidate at all, so it cannot hover and cannot take a press.
    // ======================================================================
    {
        Scene s;
        PhysicsWorld phys;
        const entt::entity canvas =
            MakePage(s, {0.0f, 0.0f, -3.0f}, kFacingCamera, 1.0f, 512.0f, 512.0f);
        const entt::entity btn = MakeButton(s, canvas, "kiosk:open");
        const entt::entity obj = MakeObject(s, {0.0f, 0.0f, -4.0f}, glm::vec3(0.4f), 8.0f, "Use");
        const entt::entity wall = MakeWall(s, {0.0f, 0.0f, -1.5f}, {2.0f, 2.0f, 0.2f});
        // Bodies are created lazily in Update - a frame must tick before any raycast
        // means anything (this bit the first draft of --test-uipick too).
        phys.Update(s, 1.0f / 60.0f);
        const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        Rig rig;
        rig.scene = &s;
        rig.cam = &cam;
        rig.actions = KeyboardInteract();
        rig.occlude = PhysicsOccluder(phys);
        rig.hasAnchor = true;
        rig.anchor = {0.0f, 0.0f, 0.0f};
        Input in;

        rig.Frame(in, true, false, /*interact*/ true);
        Check(rig.pick.kind == Hit::Kind::None,
              "4: a wall between the camera and the content did not block the pick");
        Check(!El(s, btn).hovered && !El(s, btn).clicked,
              "4: a 3D button BEHIND A WALL still hovered/activated");
        Check(rig.Object() == entt::null,
              "4: an Interactable BEHIND A WALL was still offered (the proximity "
              "fallback must be occlusion-filtered too)");
        Check(rig.pick.wallEntity == wall, "4: the occlusion cast did not name the wall");

        // Remove the wall -> both are live again (proving the block was the wall and
        // not some other rejection).
        s.Registry().destroy(wall);
        phys.Update(s, 1.0f / 60.0f);
        rig.Tap(in, true);
        Check(El(s, btn).clicked, "4b: removing the wall did not restore the 3D button");
        (void)obj;
    }

    // ======================================================================
    // 5. NEAREST WINS, in all three pairings. The affordance channel is ONE
    //    channel - one reticle, one prompt, one hover - so two winners is a bug
    //    presentation would have to tie-break.
    // ======================================================================
    {
        const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        Input in;

        // a) Two pages in line: only the nearer button reacts.
        {
            Scene s;
            const entt::entity nearC =
                MakePage(s, {0.0f, 0.0f, -3.0f}, kFacingCamera, 1.0f, 512.0f, 512.0f);
            const entt::entity farC =
                MakePage(s, {0.0f, 0.0f, -6.0f}, kFacingCamera, 2.0f, 512.0f, 512.0f);
            const entt::entity nearB = MakeButton(s, nearC, "near");
            const entt::entity farB = MakeButton(s, farC, "far");
            Rig rig;
            rig.scene = &s;
            rig.cam = &cam;
            rig.actions = KeyboardInteract();
            rig.Tap(in, true);
            Check(rig.pick.entity == nearC, "5a: the FARTHER page won the ray");
            Check(El(s, nearB).clicked, "5a: the nearer 3D button did not activate");
            Check(!El(s, farB).clicked && !El(s, farB).hovered,
                  "5a: one press activated TWO stacked 3D buttons");
        }

        // b) A page and an object in line, both orders: exactly one winner, the
        //    nearer, and the loser is ABSENT rather than merely lower priority.
        for (int order = 0; order < 2; ++order) {
            Scene s;
            const f32 pageZ = order == 0 ? -3.0f : -6.0f;
            const f32 objZ = order == 0 ? -6.0f : -3.0f;
            const entt::entity canvas =
                MakePage(s, {0.0f, 0.0f, pageZ}, kFacingCamera, 1.0f, 512.0f, 512.0f);
            const entt::entity btn = MakeButton(s, canvas, "page");
            MakeObject(s, {0.0f, 0.0f, objZ}, glm::vec3(0.4f), 10.0f, "Object");
            Rig rig;
            rig.scene = &s;
            rig.cam = &cam;
            rig.actions = KeyboardInteract();
            rig.hasAnchor = true;
            rig.anchor = {0.0f, 0.0f, 0.0f};
            rig.Tap(in, true);
            const bool pageNearer = order == 0;
            Check(rig.pick.kind ==
                      (pageNearer ? Hit::Kind::Page : Hit::Kind::Object),
                  std::string("5b: the FARTHER of a page/object pair won (page ") +
                      (pageNearer ? "near" : "far") + ")");
            Check(El(s, btn).clicked == pageNearer,
                  "5b: the 3D button reacted while an object was in front of it (or "
                  "failed to react while it was in front)");
            Check((rig.Object() != entt::null) == !pageNearer,
                  "5b: a prompt was offered for an object behind a live page");
        }

        // c) Two objects in line: the nearer is offered.
        {
            Scene s;
            const entt::entity nearO =
                MakeObject(s, {0.0f, 0.0f, -3.0f}, glm::vec3(0.4f), 10.0f, "Near");
            MakeObject(s, {0.0f, 0.0f, -6.0f}, glm::vec3(0.4f), 10.0f, "Far");
            Rig rig;
            rig.scene = &s;
            rig.cam = &cam;
            rig.actions = KeyboardInteract();
            rig.hasAnchor = true;
            rig.anchor = {0.0f, 0.0f, 0.0f};
            rig.Tap(in, true);
            Check(rig.Object() == nearO, "5c: the farther of two aimed objects won");
            Check(rig.interactPressed,
                  "5c: the Interact edge was not seen on the activation frame");
        }
    }

    // ======================================================================
    // 6. THE TERRAIN HEIGHTFIELD OCCLUDES. Terrain is a raycast surface now
    //    (--test-terraincollide), and it is the surface most content stands on -
    //    if it did not occlude, every interactable on the far side of a hill
    //    would be clickable through the hill.
    // ======================================================================
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity te = s.CreateEntity("Terrain");
        Transform tt;
        reg.emplace<Transform>(te, tt);
        TerrainComponent tc;
        tc.chunks = 2;
        tc.resolution = 16;
        tc.chunkSize = 8.0f;
        tc.height = 0.0f;
        reg.emplace<TerrainComponent>(te, tc);
        TerrainComponent& t = reg.get<TerrainComponent>(te);
        terrain::EnsureHeights(t);
        const i32 gridN = static_cast<i32>(t.GridN());
        const f32 step = terrain::SampleStep(t);
        const f32 half = terrain::ExtentXZ(t) * 0.5f;
        // A RIDGE across the middle (a wall of ground at local z = 0), 6 m tall.
        for (i32 gz = 0; gz < gridN; ++gz) {
            const f32 lz = -half + static_cast<f32>(gz) * step;
            const f32 h = std::abs(lz) < 1.2f ? 6.0f : 0.0f;
            for (i32 gx = 0; gx < gridN; ++gx)
                t.heights[static_cast<usize>(gz) * gridN + gx] = h;
        }
        terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);

        PhysicsWorld phys;
        phys.SetRunning(false); // the collider must exist without Simulate
        phys.Update(s, 1.0f / 60.0f);
        Check(t.colliderBodyId != TerrainComponent::kInvalidCollider,
              "6: the terrain grew no heightfield collider (the rest of 6 is void)");

        // Camera on the near side at head height, aiming level at content on the FAR
        // side of the ridge. The ridge is between them.
        const entt::entity canvas =
            MakePage(s, {0.0f, 1.6f, -6.0f}, kFacingCamera, 1.5f, 512.0f, 512.0f);
        const entt::entity btn = MakeButton(s, canvas, "across");
        MakeObject(s, {0.0f, 1.6f, -7.0f}, glm::vec3(0.5f), 12.0f, "Across");
        const Camera cam = FpsCam({0.0f, 1.6f, 6.0f}, {0.0f, 0.0f, -1.0f});
        Rig rig;
        rig.scene = &s;
        rig.cam = &cam;
        rig.actions = KeyboardInteract();
        rig.occlude = PhysicsOccluder(phys);
        rig.hasAnchor = true;
        rig.anchor = {0.0f, 1.6f, 6.0f};
        Input in;

        rig.Tap(in, true);
        Check(rig.pick.kind == Hit::Kind::None,
              "6: a terrain ridge did not occlude the content behind it");
        Check(rig.pick.wallEntity == te,
              "6: the blocking hit did not resolve to the TERRAIN entity");
        Check(!El(s, btn).clicked && rig.Object() == entt::null,
              "6: content on the far side of a hill was still interactive");

        // Flatten the ridge -> the same ray now reaches the same content. Proves the
        // block was the terrain SHAPE (and that a sculpt reaches the collider).
        for (i32 gz = 0; gz < gridN; ++gz)
            for (i32 gx = 0; gx < gridN; ++gx)
                t.heights[static_cast<usize>(gz) * gridN + gx] = 0.0f;
        terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);
        phys.Update(s, 1.0f / 60.0f);
        rig.Tap(in, true);
        Check(El(s, btn).clicked,
              "6b: flattening the ridge did not restore the 3D button behind it");
    }

    // ======================================================================
    // 7. STREAMED SHARD CONTENT. A shard's entities appear and disappear at
    //    runtime, and both halves of interaction have to follow: an Interactable
    //    that streams IN becomes reachable, an occluder that streams IN starts
    //    blocking, and both stop when they stream OUT (Jolt reaps despawned
    //    bodies on its next Update). This runs the REAL Streamer against a real
    //    baked level file, with a device-less Renderer.
    // ======================================================================
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "hbe_3dinteract";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path file = dir / "Kiosk.hbscene";

        std::vector<TagDef> defs;
        {
            TagDef untagged;
            untagged.name = tags::kUntaggedName;
            TagDef kiosk;
            kiosk.name = "Kiosk";
            kiosk.loadRadius = 50.0f;
            TagDef shutter;
            shutter.name = "Shutter";
            shutter.loadRadius = 50.0f;
            defs = {untagged, kiosk, shutter};
            tags::Normalize(defs);
            tags::SeedFromProject(defs);
        }
        const TagId tKiosk = tags::Intern("Kiosk"), tShutter = tags::Intern("Shutter");

        u64 gTerminal = 0, gShutter = 0;
        {
            Scene a;
            entt::registry& reg = a.Registry();
            // The interactable terminal, 3 m down -Z, in tag "Kiosk".
            const entt::entity term = MakeObject(a, {0.0f, 0.0f, -3.0f}, glm::vec3(0.5f),
                                                 10.0f, "Terminal");
            tags::Assign(reg, term, tKiosk);
            // A solid roller shutter in FRONT of it, in its own tag, so the two can
            // be streamed independently.
            const entt::entity sh = MakeWall(a, {0.0f, 0.0f, -1.5f}, {2.0f, 2.0f, 0.2f});
            reg.emplace<AABB>(sh, AABB{glm::vec3(-2.0f, -2.0f, -0.2f), glm::vec3(2.0f, 2.0f, 0.2f)});
            tags::Assign(reg, sh, tShutter);

            gTerminal = reg.get<Guid>(term).value;
            gShutter = reg.get<Guid>(sh).value;

            const tagshard::BakeReport rep = tagshard::BakeScene(a, defs);
            Check(rep.errors == 0, "7: the authored streaming level did not bake cleanly");
            Check(scene::SaveScene(a, file, {}, SceneKind::Full, &rep.shards),
                  "7: saving the baked streaming level failed");
        }

        world::Get().Clear();
        world::SetCurrentArea({});

        Renderer renderer; // device-less: uploads return invalid handles, no GPU
        Scene s;
        PhysicsWorld phys;
        stream::Streamer st;
        const bool bound = st.BindLevel(s, renderer, file, dir, defs);
        Check(bound, "7: BindLevel failed on the baked streaming level");
        const i32 iKiosk = st.FindShard("Kiosk#0"), iShutter = st.FindShard("Shutter#0");
        Check(iKiosk >= 0 && iShutter >= 0, "7: the two streamed shards are not addressable");

        if (bound && iKiosk >= 0 && iShutter >= 0) {
            const Camera cam = FpsCam({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
            Rig rig;
            rig.scene = &s;
            rig.cam = &cam;
            rig.actions = KeyboardInteract();
            rig.occlude = PhysicsOccluder(phys);
            rig.hasAnchor = true;
            rig.anchor = {0.0f, 0.0f, 0.0f};
            Input in;

            // a) Nothing spawned: nothing to interact with.
            phys.Update(s, 1.0f / 60.0f);
            rig.Tap(in, true);
            Check(rig.Object() == entt::null,
                  "7a: an unstreamed shard's Interactable was already offered");

            // b) Stream the kiosk IN -> reachable.
            Check(st.SpawnShard(s, renderer, static_cast<u32>(iKiosk)), "7b: spawn Kiosk#0");
            phys.Update(s, 1.0f / 60.0f);
            const entt::entity term = ByGuid(s, gTerminal);
            Check(term != entt::null, "7b: the spawned shard produced no terminal");
            rig.Tap(in, true);
            Check(rig.Object() == term,
                  "7b: a STREAMED-IN Interactable is not reachable by the pick pass");

            // c) Stream the shutter IN -> it blocks. This is the half that needs the
            //    physics body to exist: a streamed occluder only occludes once Jolt
            //    has created its body, which happens in the next Update.
            Check(st.SpawnShard(s, renderer, static_cast<u32>(iShutter)),
                  "7c: spawn Shutter#0");
            phys.Update(s, 1.0f / 60.0f);
            const entt::entity sh = ByGuid(s, gShutter);
            Check(sh != entt::null, "7c: the spawned shard produced no shutter");
            rig.Tap(in, true);
            Check(rig.pick.wallEntity == sh,
                  "7c: a STREAMED-IN collider is not an occluder (the pick ray does "
                  "not see shard geometry)");
            Check(rig.Object() == entt::null,
                  "7c: the terminal stayed interactive through a streamed-in shutter");

            // d) Stream the shutter OUT -> reachable again. A despawned body must
            //    stop occluding, or the world keeps invisible walls.
            Check(st.DespawnShard(s, static_cast<u32>(iShutter)), "7d: despawn Shutter#0");
            phys.Update(s, 1.0f / 60.0f);
            rig.Tap(in, true);
            Check(rig.Object() != entt::null,
                  "7d: a DESPAWNED occluder kept blocking (its Jolt body outlived it)");

            // e) Stream the kiosk OUT -> gone, and the pick must not answer for a
            //    destroyed entity out of any cache.
            Check(st.DespawnShard(s, static_cast<u32>(iKiosk)), "7e: despawn Kiosk#0");
            phys.Update(s, 1.0f / 60.0f);
            rig.Tap(in, true);
            Check(rig.pick.kind == Hit::Kind::None && rig.Object() == entt::null,
                  "7e: a DESPAWNED Interactable was still offered");
        }

        st.Reset();
        world::Get().Clear();
        world::SetCurrentArea({});
        fs::remove_all(dir, ec);
    }

    // ======================================================================
    // 8. THE POINTER POLICY itself (interact::ResolvePointer), which is the one
    //    definition the Engine and every case above both call.
    // ======================================================================
    {
        const glm::vec2 cur(0.30f, 0.70f);
        const glm::vec2 centre(0.5f, 0.5f);

        // Free cursor, mouse: the cursor drives both spaces, LMB presses.
        {
            PointerInputs p;
            p.cursorNorm = cur;
            const PointerMode m = ResolvePointer(p);
            Check(!m.reticle && !m.useInteractAction,
                  "8a: a free mouse cursor was treated as a reticle");
            Check(m.worldPointer == cur && m.screenPointer == cur,
                  "8a: a free cursor is not the pointer for both spaces");
        }
        // Locked cursor: reticle for the world, NOTHING for the screen - a
        // crosshair must not hover HUD elements it happens to sit on.
        {
            PointerInputs p;
            p.cursorLocked = true;
            p.cursorNorm = cur;
            const PointerMode m = ResolvePointer(p);
            Check(m.reticle && m.useInteractAction, "8b: a locked cursor is not the reticle");
            Check(m.worldPointer == centre, "8b: the reticle is not screen centre");
            Check(m.screenPointer.x < 0.0f,
                  "8b: the reticle reached SCREEN canvases (it must not)");
        }
        // Gamepad + free cursor: reticle anyway (a pad moves no mouse), but the
        // SCREEN pointer stays the cursor so the mouse still works alongside it.
        {
            PointerInputs p;
            p.padActive = true;
            p.cursorNorm = cur;
            const PointerMode m = ResolvePointer(p);
            Check(m.reticle && m.useInteractAction,
                  "8c: a gamepad with a free cursor could not aim at world pages");
            Check(m.worldPointer == centre && m.screenPointer == cur,
                  "8c: the pad reticle stole the SCREEN pointer from the mouse");
        }
        // ...unless a SCREEN focus ring is live. That is the pad navigating a menu,
        // and screen beats world: otherwise one press of the activate button fires
        // the focused menu button AND a world page behind it.
        {
            PointerInputs p;
            p.padActive = true;
            p.screenFocusActive = true;
            p.cursorNorm = cur;
            const PointerMode m = ResolvePointer(p);
            Check(!m.reticle && !m.useInteractAction,
                  "8d: a gamepad navigating a MENU still aimed a reticle at world "
                  "pages behind it");
        }
        // ...AND A LOCKED CURSOR DOES NOT BEAT IT. This case used to assert the
        // opposite ("a locked cursor beats that: in first-person there is no menu
        // to navigate"), on the premise that the ring might be STALE. It cannot
        // be: ui::UpdateNavigation clears `focused` the moment the element leaves
        // the layout (panel switch, scene swap, type change), so a live ring in
        // locked-cursor gameplay means a live focusable element on the HUD - and
        // one press of the Interact-bound pad button would then fire BOTH it and
        // the reticle-aimed world page. Screen space beats world space in every
        // cursor state, or the guard only half exists.
        //
        // The cost is stated plainly: a HUD that contains a focusable widget can
        // take the pad's aim away from world pages while the ring is up. That is
        // the correct trade (a double-fire is worse than a suspended reticle), and
        // a HUD of labels/images - the normal case - never raises a ring at all.
        {
            PointerInputs p;
            p.cursorLocked = true;
            p.padActive = true;
            p.screenFocusActive = true;
            const PointerMode m = ResolvePointer(p);
            Check(!m.reticle && !m.useInteractAction,
                  "8e: a live SCREEN focus ring did not suspend the locked-cursor "
                  "reticle (one press fires the HUD widget AND the world page)");
        }
        // Without the ring, a locked cursor is still the reticle.
        {
            PointerInputs p;
            p.cursorLocked = true;
            p.padActive = true;
            const PointerMode m = ResolvePointer(p);
            Check(m.reticle && m.useInteractAction,
                  "8e2: the locked-cursor reticle stopped working entirely");
        }
        // The editor feeds its own pointer over the Game image and is never a
        // reticle, whatever the cursor/pad state says.
        {
            PointerInputs p;
            p.external = true;
            p.externalNorm = cur;
            p.cursorLocked = true;
            p.padActive = true;
            const PointerMode m = ResolvePointer(p);
            Check(!m.reticle && m.worldPointer == cur && m.screenPointer == cur,
                  "8f: the editor's Game-panel pointer was overridden by a reticle");
        }
    }

    if (g_fails == 0)
        std::printf("--test-3dinteract: PASS (8 groups)\n");
    else
        std::printf("--test-3dinteract: FAILED (%d)\n", g_fails);
    return g_fails == 0;
}

} // namespace hbe::interact
