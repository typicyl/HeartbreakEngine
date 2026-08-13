// UI/UIFocus.cpp - keyboard/gamepad UI navigation + TextInput editing.
#include "UI/UIFocus.h"

#include "Core/Input.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/UISystem.h"

#include <glm/glm.hpp>

namespace hbe::ui {

namespace {

// Widgets the focus ring can land on. ScrollViews are wheel/auto-scroll only
// (focusing one would trap directional navigation inside it). P9.2: delegates to the
// WidgetVTable flag so the focusable set has ONE definition (in UISystem.cpp).
bool IsFocusable(const UIElement& el) { return WidgetIsFocusable(el); }

glm::vec2 Center(const Rect& r) {
    return {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f};
}

// The laid-out item for `e`, or nullptr when it isn't in this frame's layout
// (hidden panel, destroyed entity, structure change).
const LayoutItem* FindItem(const std::vector<LayoutItem>& layout, entt::entity e) {
    for (const LayoutItem& item : layout)
        if (item.entity == e) return &item;
    return nullptr;
}

// True when the item is a live focus candidate this frame. `modal` (P8) traps focus
// to a modal panel's subtree; entt::null = no modal = everything eligible.
bool Focusable(entt::registry& reg, const LayoutItem& item,
               entt::entity modal = entt::null) {
    if (!reg.valid(item.entity) || !reg.all_of<UIElement>(item.entity)) return false;
    if (!IsDescendantOf(reg, item.entity, modal)) return false; // modal focus trap
    const UIElement& el = reg.get<UIElement>(item.entity);
    if (!el.visible || !el.enabled || !IsFocusable(el)) return false;
    // A non-interactive UICanvasGroup subtree can't take keyboard/gamepad focus.
    if (!item.groupInteractive) return false;
    // Clipped fully out of an ancestor ScrollView = not reachable.
    if (item.hasClip && !item.clip.Contains(Center(item.rect))) return false;
    // World-space ("physical") canvases are point-and-click via ray-pick only
    // (their pixel spaces are unrelated to the screen's); keyboard/gamepad
    // focus never lands on them, so it can't jump to an off-screen page.
    if (item.canvasEntity != entt::null && reg.valid(item.canvasEntity) &&
        reg.all_of<UICanvas>(item.canvasEntity) &&
        reg.get<UICanvas>(item.canvasEntity).worldSpace)
        return false;
    return true;
}

// Directional scoring: nearest candidate along `dir` (0=up 1=down 2=left
// 3=right) with a lateral penalty; same-canvas candidates win over
// cross-canvas jumps (different canvases have unrelated pixel spaces).
entt::entity PickInDirection(entt::registry& reg, const std::vector<LayoutItem>& layout,
                             const LayoutItem& from, int dir,
                             entt::entity modal = entt::null) {
    // Authored focus graph (P8): the `from` element may name a target element `id` for
    // this direction. An override that resolves to a focusable, in-layout element wins;
    // an empty or unresolvable one falls through to the geometric pick below.
    if (const UIElement* fe = reg.try_get<UIElement>(from.entity)) {
        const std::string& ov = dir == 0   ? fe->navUp
                                : dir == 1 ? fe->navDown
                                : dir == 2 ? fe->navLeft
                                           : fe->navRight;
        if (!ov.empty())
            for (const LayoutItem& item : layout)
                if (item.entity != from.entity && Focusable(reg, item, modal) &&
                    reg.get<UIElement>(item.entity).id == ov)
                    return item.entity;
    }
    const glm::vec2 fc = Center(from.rect);
    entt::entity best = entt::null;
    f32 bestScore = 1e30f;
    for (const LayoutItem& item : layout) {
        if (item.entity == from.entity || !Focusable(reg, item, modal)) continue;
        const glm::vec2 c = Center(item.rect);
        f32 axial, lateral;
        switch (dir) {
            case 0:  axial = fc.y - c.y; lateral = glm::abs(c.x - fc.x); break;
            case 1:  axial = c.y - fc.y; lateral = glm::abs(c.x - fc.x); break;
            case 2:  axial = fc.x - c.x; lateral = glm::abs(c.y - fc.y); break;
            default: axial = c.x - fc.x; lateral = glm::abs(c.y - fc.y); break;
        }
        if (axial <= 1.0f) continue; // wrong side (or same row/column position)
        f32 score = axial + 2.0f * lateral;
        if (item.canvasEntity != from.canvasEntity) score += 100000.0f;
        if (score < bestScore) {
            bestScore = score;
            best = item.entity;
        }
    }
    return best;
}

// First candidate in reading order (top-left) - the geometric initial focus.
entt::entity PickFirst(entt::registry& reg, const std::vector<LayoutItem>& layout,
                       entt::entity modal = entt::null) {
    entt::entity best = entt::null;
    f32 bestScore = 1e30f;
    for (const LayoutItem& item : layout) {
        if (!Focusable(reg, item, modal)) continue;
        const glm::vec2 c = Center(item.rect);
        const f32 score = c.y * 4.0f + c.x; // rows dominate
        if (score < bestScore) {
            bestScore = score;
            best = item.entity;
        }
    }
    return best;
}

// Initial focus (P8): the authored firstFocus of the active modal panel, else of any
// active panel, else geometric reading-order. Only focusable + in-layout ids count.
entt::entity PickInitial(Scene& scene, const std::vector<LayoutItem>& layout,
                         entt::entity modal) {
    auto& reg = scene.Registry();
    // Resolve `id` to a focusable, in-layout element that is ALSO a descendant of
    // `panel` - so a panel's firstFocus can only land inside its OWN subtree (a
    // background HUD's firstFocus can't point at, or be confused with, another screen).
    const auto resolveIn = [&](entt::entity panel, const std::string& id) -> entt::entity {
        if (id.empty()) return entt::null;
        for (const LayoutItem& item : layout)
            if (Focusable(reg, item, modal) && IsDescendantOf(reg, item.entity, panel) &&
                reg.get<UIElement>(item.entity).id == id)
                return item.entity;
        return entt::null;
    };
    if (modal != entt::null && reg.all_of<UIPanel>(modal)) {
        if (const entt::entity e = resolveIn(modal, reg.get<UIPanel>(modal).firstFocus);
            e != entt::null)
            return e;
    } else {
        // The TOPMOST active panel (its firstFocus target latest in the draw-ordered
        // layout) that authored a resolvable firstFocus - so a persistent background
        // HUD's firstFocus can't steal the first focus from the front screen.
        entt::entity best = entt::null;
        usize bestPos = 0;
        for (const entt::entity p : reg.view<UIPanel>()) {
            const UIPanel& panel = reg.get<UIPanel>(p);
            if (!panel.active || panel.firstFocus.empty()) continue;
            const entt::entity e = resolveIn(p, panel.firstFocus);
            if (e == entt::null) continue;
            usize pos = 0;
            for (usize i = 0; i < layout.size(); ++i)
                if (layout[i].entity == e) {
                    pos = i;
                    break;
                }
            if (best == entt::null || pos >= bestPos) {
                best = e;
                bestPos = pos;
            }
        }
        if (best != entt::null) return best;
    }
    return PickFirst(reg, layout, modal);
}

// UTF-8 length in CHARACTERS (caret positions count characters, not bytes).
int CharLen(const std::string& s) {
    int n = 0;
    for (const char c : s)
        if ((static_cast<u8>(c) & 0xC0) != 0x80) ++n;
    return n;
}

// Byte offset of character index `ci` (clamped) in a UTF-8 string.
usize ByteOfChar(const std::string& s, int ci) {
    int n = 0;
    for (usize i = 0; i < s.size(); ++i) {
        if ((static_cast<u8>(s[i]) & 0xC0) != 0x80) {
            if (n == ci) return i;
            ++n;
        }
    }
    return s.size();
}

// Encode a code point as UTF-8 (the atlas rasterizes BMP glyphs).
void AppendUtf8(std::string& s, u32 cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

bool WantsTextInput(const UIContext& ctx) {
    return ctx.editing != entt::null;
}

void UpdateTooltipTimer(Scene& scene, UIContext& ctx, f32 dt, bool uiActive) {
    auto& reg = scene.Registry();
    // P9 tooltip dwell timer. The pointer pass (UpdateInteraction) put the topmost
    // tooltip-bearing element under the cursor into ctx.tooltipCandidate; advance the
    // dwell while it holds steady, reset when it moves on. BuildVertices shows the
    // popup once tooltipTime passes the element's tooltipDelay.
    //
    // Called UNCONDITIONALLY each frame (unlike UpdateNavigation, which the dev menu /
    // external ImGui keyboard owner suspends) so a shown tooltip can never freeze on
    // screen: when the game UI is not the active owner, drop it. And it takes the
    // PRESENTATION clock (dt_), not the pause-scaled simulation delta, so tooltips work
    // on the pause / settings menu - a primary tooltip surface - which is exactly where
    // the simulation clock is frozen to zero.
    if (!uiActive) {
        ctx.tooltipHover = entt::null;
        ctx.tooltipTime = 0.0f;
        return;
    }
    if (ctx.tooltipCandidate != ctx.tooltipHover) {
        ctx.tooltipHover = ctx.tooltipCandidate;
        ctx.tooltipTime = 0.0f;
    } else if (ctx.tooltipHover != entt::null) {
        ctx.tooltipTime += dt;
    }
    if (ctx.tooltipHover != entt::null &&
        (!reg.valid(ctx.tooltipHover) || !reg.all_of<UIElement>(ctx.tooltipHover))) {
        ctx.tooltipHover = entt::null;
        ctx.tooltipTime = 0.0f;
    }
}

void UpdateNavigation(Scene& scene, const Input& input, UIContext& ctx, f32 dt) {
    auto& reg = scene.Registry();
    // P8 modal scope: focus is trapped to the topmost SHOWN modal panel's subtree.
    const entt::entity modal = ActiveModalPanel(scene, ctx.layout);

    // Validate carried-over focus: clear it if the entity died, left the layout
    // (panel switch, scene swap), is no longer focusable (its type changed), or now
    // sits OUTSIDE an active modal (a dialog just opened over it) - so a stale focus
    // ring never lingers on a non-interactive or now-blocked element.
    if (ctx.focused != entt::null) {
        const LayoutItem* fi = FindItem(ctx.layout, ctx.focused);
        if (!reg.valid(ctx.focused) || !reg.all_of<UIElement>(ctx.focused) || !fi ||
            !IsFocusable(reg.get<UIElement>(ctx.focused)) ||
            !IsDescendantOf(reg, ctx.focused, modal)) {
            ctx.focused = entt::null;
        }
    }
    // Validate the edit session. If the element is gone entirely, just drop it.
    // If it still exists but left the layout (its panel was hidden mid-edit),
    // CANCEL the session (restore the pre-edit text) - leaving half-typed text
    // that never fired `changed` would silently diverge the widget from the
    // value the game applied.
    if (ctx.editing != entt::null) {
        if (!reg.valid(ctx.editing) || !reg.all_of<UIElement>(ctx.editing)) {
            ctx.editing = entt::null;
        } else if (!FindItem(ctx.layout, ctx.editing)) {
            reg.get<UIElement>(ctx.editing).text = ctx.preEditText;
            ctx.editing = entt::null;
        }
    }

    const Input::GamepadState& pad = input.Gamepad(0);

    // Helper: begin a TextInput edit session.
    const auto beginEdit = [&](entt::entity e) {
        UIElement& el = reg.get<UIElement>(e);
        ctx.editing = e;
        ctx.focused = e;
        ctx.preEditText = el.text;
        ctx.caretPos = CharLen(el.text);
        ctx.caretBlink = 0.0f;
    };

    // ---- Active edit session: typed characters + caret editing -------------
    if (ctx.editing != entt::null) {
        UIElement& el = reg.get<UIElement>(ctx.editing);
        ctx.caretBlink += dt;
        bool touched = false;
        const int maxChars = glm::max(el.maxLength, 1);

        // Replay this frame's text events in message order (insertions and
        // caret/delete keys interleaved). Ordering matters: a backspace-then-
        // type in one frame must delete THEN insert; a two-channel approach
        // (all chars, then all deletes) would corrupt the result.
        for (u32 i = 0; i < input.TextEventCount(); ++i) {
            const Input::TextEditEvent& ev = input.TextEvent(i);
            switch (ev.kind) {
                case Input::TextEditEvent::InsertChar:
                    if (CharLen(el.text) < maxChars) {
                        std::string enc;
                        AppendUtf8(enc, ev.codepoint);
                        el.text.insert(ByteOfChar(el.text, ctx.caretPos), enc);
                        ++ctx.caretPos;
                        touched = true;
                    }
                    break;
                case Input::TextEditEvent::Backspace:
                    if (ctx.caretPos > 0) {
                        const usize b0 = ByteOfChar(el.text, ctx.caretPos - 1);
                        const usize b1 = ByteOfChar(el.text, ctx.caretPos);
                        el.text.erase(b0, b1 - b0);
                        --ctx.caretPos;
                        touched = true;
                    }
                    break;
                case Input::TextEditEvent::Delete:
                    if (ctx.caretPos < CharLen(el.text)) {
                        const usize b0 = ByteOfChar(el.text, ctx.caretPos);
                        const usize b1 = ByteOfChar(el.text, ctx.caretPos + 1);
                        el.text.erase(b0, b1 - b0);
                        touched = true;
                    }
                    break;
                case Input::TextEditEvent::CaretLeft:
                    ctx.caretPos = glm::max(ctx.caretPos - 1, 0);
                    break;
                case Input::TextEditEvent::CaretRight:
                    ctx.caretPos = glm::min(ctx.caretPos + 1, CharLen(el.text));
                    break;
                case Input::TextEditEvent::CaretHome:
                    ctx.caretPos = 0;
                    break;
                case Input::TextEditEvent::CaretEnd:
                    ctx.caretPos = CharLen(el.text);
                    break;
            }
        }
        ctx.caretPos = glm::clamp(ctx.caretPos, 0, CharLen(el.text));
        if (touched) ctx.caretBlink = 0.0f; // typing keeps the caret solid

        // Commit: Enter / gamepad A. Cancel: Escape / gamepad B (restores the
        // pre-edit snapshot). A click elsewhere commits too (the element's own
        // click was consumed to *start* the session, so el.clicked means
        // re-click = keep editing).
        const bool commit = input.WasKeyPressedRaw(Key::Enter) ||
                            (pad.connected && pad.WasPressed(Gamepad_A)) ||
                            (input.WasMousePressed(MouseButton::Left) && !el.clicked);
        const bool cancel = input.WasKeyPressedRaw(Key::Escape) ||
                            (pad.connected && pad.WasPressed(Gamepad_B));
        if (cancel) {
            el.text = ctx.preEditText;
            ctx.editing = entt::null;
        } else if (commit) {
            el.changed = true; // settings bindings + schematic events fire
            ctx.editing = entt::null;
            ctx.interactives.push_back(ctx.focused); // flag clears next frame
            // A click that landed on ANOTHER TextInput commits this session and
            // immediately starts editing that one (no dead click).
            for (const LayoutItem& item : ctx.layout) {
                if (item.entity == ctx.focused || !Focusable(reg, item, modal)) continue;
                UIElement& other = reg.get<UIElement>(item.entity);
                if (other.clicked && other.type == UIElement::Type::TextInput) {
                    beginEdit(item.entity);
                    break;
                }
            }
        }
        return; // an edit session consumes navigation entirely
    }

    // ---- Mouse interplay ----------------------------------------------------
    // Hover moves focus (and hides the ring - the cursor is the indicator), but
    // ONLY when the mouse actually moved this frame: `hovered` is a level state
    // (re-asserted every frame from the resting pointer), so an unconditional
    // steal would yank focus back to a parked cursor and make keyboard/gamepad
    // navigation impossible. Clicking a TextInput always starts editing (a
    // click is a real event, not a resting state).
    const bool mouseMoved =
        input.MouseDeltaX() != 0.0f || input.MouseDeltaY() != 0.0f;
    for (const LayoutItem& item : ctx.layout) {
        if (!Focusable(reg, item, modal)) continue;
        UIElement& el = reg.get<UIElement>(item.entity);
        if (el.hovered && mouseMoved) {
            ctx.focused = item.entity;
            ctx.focusVisible = false;
        }
        if (el.clicked && el.type == UIElement::Type::TextInput) {
            beginEdit(item.entity);
            return;
        }
    }

    // ---- Directional navigation (arrows / D-pad / left stick) --------------
    int dir = -1; // 0=up 1=down 2=left 3=right; highest held wins
    if (input.IsKeyDown(Key::Up) || (pad.connected && pad.IsDown(Gamepad_DPadUp)) ||
        (pad.connected && pad.leftY > 0.55f))
        dir = 0;
    else if (input.IsKeyDown(Key::Down) ||
             (pad.connected && pad.IsDown(Gamepad_DPadDown)) ||
             (pad.connected && pad.leftY < -0.55f))
        dir = 1;
    else if (input.IsKeyDown(Key::Left) ||
             (pad.connected && pad.IsDown(Gamepad_DPadLeft)) ||
             (pad.connected && pad.leftX < -0.55f))
        dir = 2;
    else if (input.IsKeyDown(Key::Right) ||
             (pad.connected && pad.IsDown(Gamepad_DPadRight)) ||
             (pad.connected && pad.leftX > 0.55f))
        dir = 3;

    // Hold-to-repeat: first move fires immediately, then repeats after a
    // typematic delay (works uniformly for keys, D-pad, and the stick).
    bool navFire = false;
    if (dir != ctx.navHeldDir) {
        ctx.navHeldDir = dir;
        if (dir >= 0) {
            navFire = true;
            ctx.navRepeat = 0.4f;
        }
    } else if (dir >= 0) {
        ctx.navRepeat -= dt;
        if (ctx.navRepeat <= 0.0f) {
            navFire = true;
            ctx.navRepeat = 0.12f;
        }
    }

    if (navFire) {
        ctx.focusVisible = true;
        if (ctx.focused == entt::null) {
            ctx.focused = PickInitial(scene, ctx.layout, modal); // authored first-focus, else geometric
        } else if (const LayoutItem* from = FindItem(ctx.layout, ctx.focused)) {
            UIElement& el = reg.get<UIElement>(ctx.focused);
            // Value widgets consume left/right to adjust; up/down always navigates. At the
            // value's END (slider clamp, selector edge) the direction is NOT consumed - it
            // falls through to navigation so focus can escape in a single-row layout (no
            // soft-lock). P9.2: the per-type value change is the WidgetVTable's onNav.
            const bool consumed = WidgetNav(el, dir);
            if (consumed) ctx.interactives.push_back(ctx.focused);
            if (!consumed) {
                const entt::entity next = PickInDirection(reg, ctx.layout, *from, dir, modal);
                if (next != entt::null) ctx.focused = next;
            }
        }
    }

    // ---- Activation (Enter / gamepad A) -------------------------------------
    const bool activate = input.WasKeyPressed(Key::Enter) ||
                          (pad.connected && pad.WasPressed(Gamepad_A));
    if (activate && ctx.focused != entt::null && reg.valid(ctx.focused) &&
        reg.all_of<UIElement>(ctx.focused)) {
        UIElement& el = reg.get<UIElement>(ctx.focused);
        ctx.focusVisible = true;
        // P9.2: per-type activation (click / toggle / advance) lives in the WidgetVTable;
        // a TextInput asks for an edit session, and beginEdit stays here since it owns the
        // UIContext edit state (editing/caret/preEditText).
        if (WidgetActivate(el)) beginEdit(ctx.focused);
        ctx.interactives.push_back(ctx.focused);
    }
}

} // namespace hbe::ui
