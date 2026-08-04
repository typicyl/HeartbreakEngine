// Core/Input.h - platform-agnostic input state (keyboard, mouse, gamepad).
//
// The platform window translates native events (Win32 messages, XInput polls)
// into this state; game/editor code queries it without touching the OS or any
// UI library. Per-frame edge state (pressed/released, mouse delta, wheel) is
// reset by NewFrame(), which the engine calls once per frame *before* pumping
// OS messages, so events accumulate into the frame that consumes them.
#pragma once

#include "Core/Types.h"

namespace hbe {

// Shared input math that is not the operating system's business. Lives here so a second
// backend reuses the exact curve rather than re-deriving it - the deadzone shape is felt
// directly by the player, and two backends disagreeing on it is a bug nobody would think
// to look for. Implemented in Core/Input.cpp; see there for why it is radial.
namespace input_detail {
void NormalizeStick(i16 rawX, i16 rawY, i32 deadzone, f32& outX, f32& outY);
} // namespace input_detail

// Keyboard keys (a practical subset; extend as needed).
enum class Key : u8 {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, Space, Enter, Backspace, Delete, Insert,
    Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    Shift, Ctrl, Alt,
    Grave, // backtick / tilde key (VK_OEM_3) - dev menu toggle
    Count
};

enum class MouseButton : u8 {
    Left = 0,
    Right,
    Middle,
    X1,
    X2,
    Count
};

// Gamepad button bitmask (mirrors XInput's layout).
enum GamepadButton : u32 {
    Gamepad_DPadUp    = 0x0001,
    Gamepad_DPadDown  = 0x0002,
    Gamepad_DPadLeft  = 0x0004,
    Gamepad_DPadRight = 0x0008,
    Gamepad_Start     = 0x0010,
    Gamepad_Back      = 0x0020,
    Gamepad_LThumb    = 0x0040,
    Gamepad_RThumb    = 0x0080,
    Gamepad_LShoulder = 0x0100,
    Gamepad_RShoulder = 0x0200,
    Gamepad_A         = 0x1000,
    Gamepad_B         = 0x2000,
    Gamepad_X         = 0x4000,
    Gamepad_Y         = 0x8000,
};

class Input : public NonCopyable {
public:
    static constexpr u32 kMaxGamepads = 4;

    // One text-editing action this frame. WM_CHAR insertions and editing keys
    // (backspace/delete/caret) are recorded into a SINGLE ordered stream in
    // Win32 message order, so a text field replays them faithfully: a
    // backspace-then-type in one frame resolves correctly, OS auto-repeat
    // produces one event per repeat, and a same-frame tap is never lost.
    struct TextEditEvent {
        enum Kind : u8 {
            InsertChar, // `codepoint` typed
            Backspace,
            Delete,
            CaretLeft,
            CaretRight,
            CaretHome,
            CaretEnd
        };
        Kind kind = InsertChar;
        u32 codepoint = 0; // valid when kind == InsertChar
    };

    struct GamepadState {
        bool connected = false;
        // Sticks in [-1,1] (deadzone applied, +Y = up), triggers in [0,1].
        f32 leftX = 0, leftY = 0;
        f32 rightX = 0, rightY = 0;
        f32 leftTrigger = 0, rightTrigger = 0;
        u32 buttons = 0;
        u32 prevButtons = 0;

        bool IsDown(GamepadButton b) const { return (buttons & b) != 0; }
        bool WasPressed(GamepadButton b) const {
            return (buttons & b) != 0 && (prevButtons & b) == 0;
        }
    };

    // Rolls per-frame state: edge detection snapshots, mouse delta/wheel reset,
    // gamepad poll. Call once per frame before pumping OS messages.
    void NewFrame();

    // -- Platform event sink (called by the platform window) -----------------
    void OnKeyVK(u32 nativeKey, bool down); // native = Win32 virtual-key code
    // The platform-independent entry point the above translates INTO. A backend that
    // already speaks hbe::Key calls this directly; Key::Unknown is ignored rather than
    // used as an index.
    void OnKey(Key k, bool down);
    void OnChar(u32 codepoint);    // translated text input (WM_CHAR), printable only
    void OnMouseButton(MouseButton b, bool down);
    void OnMouseMove(f32 x, f32 y);
    void OnMouseLockedDelta(f32 dx, f32 dy); // cursor-locked mouse-look: raw delta
    void ResetMousePos();          // drop the last position (no delta on next move)
    void OnMouseWheel(f32 delta);  // +1 per notch away from the user
    void OnFocusLost();            // releases all held state

    // -- Keyboard -------------------------------------------------------------
    // While a UI text field is being edited (SetTextCapture(true)), the normal
    // keyboard queries read as released so gameplay/dev-menu/schematic keys
    // don't fire mid-typing. The ...Raw variants ignore the capture - the text
    // editing code itself uses those. Mouse and gamepad are never captured.
    //
    // WasKeyPressed[Raw] is a true down-EDGE: it fires for a key whose press and
    // release both land in one (long/hitchy) frame, which the old
    // keys && !prevKeys form silently dropped.
    bool IsKeyDown(Key k) const      { return !textCapture_ && keys_[Index(k)]; }
    bool WasKeyPressed(Key k) const  { return !textCapture_ && pressedEdge_[Index(k)]; }
    bool WasKeyReleased(Key k) const {
        return !textCapture_ && !keys_[Index(k)] && prevKeys_[Index(k)];
    }
    bool IsKeyDownRaw(Key k) const     { return keys_[Index(k)]; }
    bool WasKeyPressedRaw(Key k) const { return pressedEdge_[Index(k)]; }
    void SetTextCapture(bool on) { textCapture_ = on; }
    bool TextCaptured() const    { return textCapture_; }

    // Ordered text-editing events this frame (insertions interleaved with
    // caret/delete keys in message order); drained by the focused UI text field.
    u32 TextEventCount() const { return textEventCount_; }
    const TextEditEvent& TextEvent(u32 i) const {
        return textEvents_[i < textEventCount_ ? i : 0];
    }

    // -- Mouse ----------------------------------------------------------------
    bool IsMouseDown(MouseButton b) const      { return mouse_[Index(b)]; }
    bool WasMousePressed(MouseButton b) const  { return mouse_[Index(b)] && !prevMouse_[Index(b)]; }
    bool WasMouseReleased(MouseButton b) const { return !mouse_[Index(b)] && prevMouse_[Index(b)]; }
    f32 MouseX() const      { return mouseX_; }
    f32 MouseY() const      { return mouseY_; }
    f32 MouseDeltaX() const { return mouseDeltaX_; }
    f32 MouseDeltaY() const { return mouseDeltaY_; }
    f32 MouseWheel() const  { return wheel_; }

    // -- Gamepad ---------------------------------------------------------------
    const GamepadState& Gamepad(u32 index = 0) const {
        return pads_[index < kMaxGamepads ? index : 0];
    }

    // TEST SEAM. Headless self-tests have no XInput device, so there is no other
    // way to exercise a gamepad-driven code path (--test-3dinteract's gamepad
    // activation case). Both masks are explicit so the caller owns the EDGE:
    // WasPressed is `buttons & ~prev`, so hold is inject(B,B) and press is
    // inject(B,0). Call it AFTER NewFrame(), which polls and would overwrite it.
    void InjectGamepadForTest(u32 buttons, u32 prev, u32 index = 0) {
        GamepadState& p = pads_[index < kMaxGamepads ? index : 0];
        p.connected = true;
        p.prevButtons = prev;
        p.buttons = buttons;
        if (buttons != 0) lastGamepad_ = true;
    }

    // TEST SEAM (sticks). Same reason and same rules as the button injector
    // above - call it AFTER NewFrame(), which polls XInput and would overwrite
    // it. Values are post-deadzone, in the same [-1,1] the poll produces (+Y up).
    // --test-fpslook drives camera look through this.
    void InjectGamepadSticksForTest(f32 lx, f32 ly, f32 rx, f32 ry, u32 index = 0) {
        GamepadState& p = pads_[index < kMaxGamepads ? index : 0];
        p.connected = true;
        p.leftX = lx;
        p.leftY = ly;
        p.rightX = rx;
        p.rightY = ry;
    }

    // -- Active device (for adaptive prompts) ---------------------------------
    // Which controller family a connected gamepad is, so button prompts can show
    // the right glyph. Read via a raw-input vendor-id scan (XInput can't tell
    // brands apart); defaults to Xbox when no HID game controller is identified.
    enum class PadBrand : u8 { Xbox = 0, PlayStation, Nintendo, Generic };
    // True if the most recent input came from a gamepad (vs keyboard/mouse); flips
    // live as the player switches devices, so the UI can swap prompts to match.
    bool LastInputWasGamepad() const { return lastGamepad_; }
    PadBrand GamepadBrand() const { return padBrand_; }

private:
    static usize Index(Key k) { return static_cast<usize>(k); }
    static usize Index(MouseButton b) { return static_cast<usize>(b); }

    void PollGamepads(); // platform-specific (XInput on Windows)
    void RefreshGamepadBrand(); // platform-specific: raw-input vendor-id scan
    void PushEditKey(Key k); // append an editing key to the text-event stream

    static constexpr u32 kMaxTextEvents = 128; // text edits buffered per frame

    bool keys_[static_cast<usize>(Key::Count)] = {};
    bool prevKeys_[static_cast<usize>(Key::Count)] = {};
    bool pressedEdge_[static_cast<usize>(Key::Count)] = {}; // down-transition this frame
    bool mouse_[static_cast<usize>(MouseButton::Count)] = {};
    bool prevMouse_[static_cast<usize>(MouseButton::Count)] = {};

    TextEditEvent textEvents_[kMaxTextEvents]; // ordered text edits this frame
    u32 textEventCount_ = 0;
    bool textCapture_ = false;  // a UI text field owns the keyboard

    f32 mouseX_ = 0, mouseY_ = 0;
    f32 mouseDeltaX_ = 0, mouseDeltaY_ = 0;
    f32 wheel_ = 0;
    bool hasMousePos_ = false; // first move seeds position without a delta spike

    GamepadState pads_[kMaxGamepads];
    u32 padRetryCooldown_[kMaxGamepads] = {}; // frames until re-probing a disconnected pad
    bool lastGamepad_ = false;                // most recent input came from a gamepad
    PadBrand padBrand_ = PadBrand::Xbox;      // detected controller family (default Xbox)
    u32 padBrandCooldown_ = 0;                // frames until the next brand rescan
};

bool InputSelfTest(); // --test-input

} // namespace hbe
