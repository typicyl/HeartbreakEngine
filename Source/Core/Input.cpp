// Core/Input.cpp - the part of Input that is not about any operating system.
//
// WHY THIS FILE EXISTS. All of Input lived in Input_Win32.cpp, and most of it had nothing to
// do with Win32: press-edge detection, the ordered text-edit stream, mouse delta accumulation,
// focus-loss clearing, and the thumbstick deadzone curve are the same on every platform. A
// second backend would have had to either copy them or link a file named "_Win32".
//
// What is left in Input_Win32.cpp after this split is exactly the OS-shaped part: translating
// native virtual-key codes, and asking XInput/Raw Input about gamepads.
//
// This split also made the interesting half TESTABLE. None of the logic below needs a window,
// a message pump, or a device, so --test-input can drive it directly - and it immediately
// covered behaviour that had never been exercised, like a press and release inside one frame.
#include "Core/Input.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace hbe {

namespace input_detail {

void NormalizeStick(i16 rawX, i16 rawY, i32 deadzone, f32& outX, f32& outY) {
    const f32 x = static_cast<f32>(rawX);
    const f32 y = static_cast<f32>(rawY);
    const f32 mag = std::sqrt(x * x + y * y);
    if (mag <= static_cast<f32>(deadzone)) {
        outX = outY = 0.0f;
        return;
    }
    // RADIAL, not per-axis. A per-axis deadzone makes the neutral region a square, so a
    // diagonal push registers sooner than a straight one and slow circular motion visibly
    // snaps to the axes. Rescaling the magnitude past the deadzone also means the stick
    // starts from zero rather than jumping to the deadzone value the instant it is crossed.
    const f32 norm = (mag - static_cast<f32>(deadzone)) / (32767.0f - static_cast<f32>(deadzone));
    const f32 scale = (norm > 1.0f ? 1.0f : norm) / mag;
    outX = x * scale;
    outY = y * scale;
}

} // namespace input_detail

void Input::NewFrame() {
    std::memcpy(prevKeys_, keys_, sizeof(keys_));
    std::memcpy(prevMouse_, mouse_, sizeof(mouse_));
    std::memset(pressedEdge_, 0, sizeof(pressedEdge_));
    mouseDeltaX_ = mouseDeltaY_ = 0.0f;
    wheel_ = 0.0f;
    textEventCount_ = 0;
    PollGamepads(); // the one platform call in the frame path
}

void Input::PushEditKey(Key k) {
    TextEditEvent::Kind kind;
    switch (k) {
        case Key::Backspace: kind = TextEditEvent::Backspace; break;
        case Key::Delete:    kind = TextEditEvent::Delete; break;
        case Key::Left:      kind = TextEditEvent::CaretLeft; break;
        case Key::Right:     kind = TextEditEvent::CaretRight; break;
        case Key::Home:      kind = TextEditEvent::CaretHome; break;
        case Key::End:       kind = TextEditEvent::CaretEnd; break;
        default: return; // not an editing key
    }
    if (textEventCount_ < kMaxTextEvents)
        textEvents_[textEventCount_++] = TextEditEvent{kind, 0};
}

void Input::OnKey(Key k, bool down) {
    if (k == Key::Unknown) return;
    if (down) {
        lastGamepad_ = false; // deliberate keyboard input -> keyboard/mouse prompts
        // Down-transition = a fresh press edge (survives a same-frame release).
        if (!keys_[Index(k)]) pressedEdge_[Index(k)] = true;
        // Editing keys feed the ordered text-event stream, one per key-down - so OS auto-
        // repeat yields repeated edits and message order relative to character insertions
        // is preserved. Cleared each frame; only drained by an active text-field session.
        PushEditKey(k);
    }
    keys_[Index(k)] = down;
}

void Input::OnChar(u32 codepoint) {
    if (textEventCount_ < kMaxTextEvents)
        textEvents_[textEventCount_++] = TextEditEvent{TextEditEvent::InsertChar, codepoint};
}

void Input::OnMouseButton(MouseButton b, bool down) {
    if (down) lastGamepad_ = false;
    mouse_[Index(b)] = down;
}

void Input::OnMouseMove(f32 x, f32 y) {
    if (hasMousePos_) {
        mouseDeltaX_ += x - mouseX_;
        mouseDeltaY_ += y - mouseY_;
        // Notable mouse movement -> the player is on mouse (not a resting cursor while
        // playing on a pad); flip prompts back to keyboard/mouse.
        if (std::fabs(x - mouseX_) + std::fabs(y - mouseY_) > 2.0f) lastGamepad_ = false;
    }
    mouseX_ = x;
    mouseY_ = y;
    hasMousePos_ = true;
}

void Input::OnMouseLockedDelta(f32 dx, f32 dy) {
    mouseDeltaX_ += dx;
    mouseDeltaY_ += dy;
}

void Input::ResetMousePos() {
    hasMousePos_ = false; // the next OnMouseMove seeds position without a delta
}

void Input::OnMouseWheel(f32 delta) {
    wheel_ += delta;
}

void Input::OnFocusLost() {
    // Everything held must be released. Without this, alt-tabbing away mid-strafe leaves the
    // key latched down and the character walks into a wall until the player clicks back in.
    std::memset(keys_, 0, sizeof(keys_));
    std::memset(mouse_, 0, sizeof(mouse_));
    std::memset(pressedEdge_, 0, sizeof(pressedEdge_));
    textEventCount_ = 0;
    hasMousePos_ = false;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("input FAIL: %s\n", what);
        ++g_fails;
    }
}
bool Near(f32 a, f32 b, f32 eps = 0.0005f) { return std::fabs(a - b) <= eps; }
} // namespace

bool InputSelfTest() {
    g_fails = 0;
    Input in;

    // -- Press edges ---------------------------------------------------------
    // NewFrame polls gamepads, which is a real platform call here; that is fine and is
    // itself worth exercising - it must not disturb keyboard state.
    in.NewFrame();
    in.OnKey(Key::Space, true);
    Check(in.IsKeyDown(Key::Space), "a key reported down must read as down");
    Check(in.WasKeyPressed(Key::Space), "...and as pressed on the frame it went down");

    in.NewFrame();
    Check(in.IsKeyDown(Key::Space), "a held key stays down across frames");
    Check(!in.WasKeyPressed(Key::Space), "...but only edges ONCE - held is not pressed");

    in.OnKey(Key::Space, false);
    Check(!in.IsKeyDown(Key::Space), "a released key reads as up");

    // THE CASE THE EDGE ARRAY EXISTS FOR. A press and release inside one frame would be
    // invisible to a prev-vs-current comparison: both samples read "up". A jump tapped
    // during a hitch frame must still register.
    in.NewFrame();
    in.OnKey(Key::Enter, true);
    in.OnKey(Key::Enter, false);
    Check(!in.IsKeyDown(Key::Enter), "a key pressed and released in one frame ends up");
    Check(in.WasKeyPressed(Key::Enter),
          "A PRESS AND RELEASE INSIDE ONE FRAME MUST STILL COUNT AS A PRESS - otherwise a "
          "tap during a long frame is silently swallowed");

    in.NewFrame();
    Check(!in.WasKeyPressed(Key::Enter), "...and does not repeat on the following frame");

    // An untranslatable native key must be dropped, not indexed with.
    in.OnKey(Key::Unknown, true);
    Check(!in.WasKeyPressed(Key::Unknown), "an unknown key must be ignored entirely");

    // -- Focus loss ----------------------------------------------------------
    in.NewFrame();
    in.OnKey(Key::W, true);
    in.OnMouseButton(MouseButton::Left, true);
    in.OnFocusLost();
    Check(!in.IsKeyDown(Key::W),
          "ALT-TAB MUST RELEASE HELD KEYS - a latched movement key walks the character into "
          "a wall until the window is clicked back");
    Check(!in.IsMouseDown(MouseButton::Left), "...and held mouse buttons");
    Check(in.TextEventCount() == 0, "...and drops buffered text edits");

    // -- Mouse deltas --------------------------------------------------------
    in.NewFrame();
    in.ResetMousePos();
    in.OnMouseMove(100.0f, 100.0f);
    Check(Near(in.MouseDeltaX(), 0.0f) && Near(in.MouseDeltaY(), 0.0f),
          "THE FIRST MOVE AFTER A WARP MUST NOT PRODUCE A DELTA - seeding position from a "
          "stale origin is what makes the camera snap on mouse capture");
    in.OnMouseMove(110.0f, 90.0f);
    Check(Near(in.MouseDeltaX(), 10.0f) && Near(in.MouseDeltaY(), -10.0f),
          "a subsequent move produces the delta from the previous position");
    in.OnMouseMove(115.0f, 90.0f);
    Check(Near(in.MouseDeltaX(), 15.0f), "deltas ACCUMULATE within a frame, they do not replace");
    in.NewFrame();
    Check(Near(in.MouseDeltaX(), 0.0f), "...and reset at the frame boundary");

    in.OnMouseLockedDelta(3.0f, 4.0f);
    in.OnMouseLockedDelta(1.0f, 1.0f);
    Check(Near(in.MouseDeltaX(), 4.0f) && Near(in.MouseDeltaY(), 5.0f),
          "locked-mode raw deltas accumulate too");

    in.NewFrame();
    in.OnMouseWheel(1.0f);
    in.OnMouseWheel(-0.5f);
    Check(Near(in.MouseWheel(), 0.5f), "wheel notches accumulate within a frame");
    in.NewFrame();
    Check(Near(in.MouseWheel(), 0.0f), "...and reset");

    // -- Text stream ---------------------------------------------------------
    // ORDER IS THE POINT. Character insertions and editing keys arrive as separate OS
    // messages; a text field replaying them out of order corrupts what the user typed.
    in.NewFrame();
    in.OnChar('a');
    in.OnKey(Key::Backspace, true);
    in.OnChar('b');
    in.OnKey(Key::Left, true);
    {
        Check(in.TextEventCount() == 4, "every character and editing key must reach the stream");
        if (in.TextEventCount() == 4) {
            const Input::TextEditEvent ev[4] = {in.TextEvent(0), in.TextEvent(1),
                                                in.TextEvent(2), in.TextEvent(3)};
            Check(ev[0].kind == Input::TextEditEvent::InsertChar && ev[0].codepoint == 'a',
                  "the first insertion arrives first");
            Check(ev[1].kind == Input::TextEditEvent::Backspace,
                  "AN EDITING KEY MUST KEEP ITS PLACE AMONG INSERTIONS - replaying them out "
                  "of order corrupts the typed text");
            Check(ev[2].kind == Input::TextEditEvent::InsertChar && ev[2].codepoint == 'b', "then 'b'");
            Check(ev[3].kind == Input::TextEditEvent::CaretLeft, "then the caret move");
        }
    }
    in.NewFrame();
    Check(in.TextEventCount() == 0, "the stream is per-frame and drains at the boundary");

    // A key that is not an editing key must not enter the stream at all.
    in.OnKey(Key::W, true);
    Check(in.TextEventCount() == 0, "an ordinary key contributes no text event");

    // The buffer is bounded, and overflowing it must drop rather than overrun.
    in.NewFrame();
    for (u32 i = 0; i < 500; ++i) in.OnChar('x');
    Check(in.TextEventCount() <= 128, "THE TEXT BUFFER MUST BE BOUNDED, not overrun");

    // -- Text capture --------------------------------------------------------
    // While a text field is being edited, keys must NOT also drive the game. Without this
    // gate, typing a level name into a field walks the camera and fires whatever W, A, S
    // and D are bound to.
    in.NewFrame();
    in.OnKey(Key::W, true);
    Check(in.IsKeyDown(Key::W), "a key drives the game normally");
    in.SetTextCapture(true);
    Check(!in.IsKeyDown(Key::W),
          "WHILE A TEXT FIELD IS FOCUSED, KEYS MUST NOT DRIVE THE GAME - otherwise typing "
          "a name into a field also walks the camera");
    Check(!in.WasKeyPressed(Key::W), "...including press edges");
    Check(in.IsMouseDown(MouseButton::Left) == false, "mouse state is unaffected by capture");
    in.SetTextCapture(false);
    Check(in.IsKeyDown(Key::W), "and the key is still physically held once capture ends");
    in.OnKey(Key::W, false);

    // -- Active device -------------------------------------------------------
    in.NewFrame();
    in.InjectGamepadForTest(0x1000, 0);
    Check(in.LastInputWasGamepad(), "gamepad activity selects controller prompts");
    in.OnKey(Key::W, true);
    Check(!in.LastInputWasGamepad(), "...and a keypress switches back to keyboard prompts");
    in.InjectGamepadForTest(0x1000, 0);
    in.OnMouseMove(0.0f, 0.0f);
    in.OnMouseMove(40.0f, 40.0f);
    Check(!in.LastInputWasGamepad(), "a real mouse MOVE also switches back");
    in.InjectGamepadForTest(0x1000, 0);
    in.OnMouseMove(40.5f, 40.0f);
    Check(in.LastInputWasGamepad(),
          "...but a resting cursor's jitter must NOT - that would flicker the on-screen "
          "prompts between mouse and pad while playing on a pad");

    // -- Deadzone curve ------------------------------------------------------
    {
        f32 x = 1.0f, y = 1.0f;
        input_detail::NormalizeStick(0, 0, 7849, x, y);
        Check(Near(x, 0.0f) && Near(y, 0.0f), "a centred stick reads exactly zero");

        input_detail::NormalizeStick(5000, 0, 7849, x, y);
        Check(Near(x, 0.0f) && Near(y, 0.0f), "inside the deadzone reads zero");

        // CONTINUITY: just past the deadzone must start from ~0, not jump to the deadzone
        // fraction. A discontinuity here is felt directly as a lurch off centre.
        input_detail::NormalizeStick(7900, 0, 7849, x, y);
        Check(x > 0.0f && x < 0.02f, "just past the deadzone starts from near zero, no jump");

        input_detail::NormalizeStick(32767, 0, 7849, x, y);
        Check(Near(x, 1.0f) && Near(y, 0.0f), "a fully deflected axis reaches exactly 1");

        // RADIAL, not square: a full diagonal must not exceed unit length, or diagonal
        // movement is faster than straight movement.
        input_detail::NormalizeStick(32767, 32767, 7849, x, y);
        const f32 mag = std::sqrt(x * x + y * y);
        Check(mag <= 1.0001f,
              "A FULL DIAGONAL MUST NOT EXCEED UNIT LENGTH - a square deadzone makes "
              "diagonal movement faster than straight movement");
        Check(Near(x, y), "...and stays symmetric on the diagonal");

        input_detail::NormalizeStick(-32767, 0, 7849, x, y);
        Check(Near(x, -1.0f), "negative deflection is symmetric");
    }

    if (g_fails == 0)
        std::printf("input: press edges survive a press+release in ONE frame, focus loss "
                    "releases everything held, the first move after a warp makes no delta, "
                    "the text stream keeps insertions and editing keys in ORDER within a "
                    "bounded buffer, prompt switching ignores cursor jitter, and the stick "
                    "deadzone is radial and continuous off centre\n");
    return g_fails == 0;
}

} // namespace hbe
