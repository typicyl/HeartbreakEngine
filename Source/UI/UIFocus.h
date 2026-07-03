// UI/UIFocus.h - keyboard/gamepad UI navigation + TextInput editing.
//
// UpdateNavigation runs each frame right after ui::UpdateInteraction. It moves
// a focus ring between interactive elements (arrow keys, gamepad D-pad, left
// stick), activates the focused widget (Enter / gamepad A) by setting the SAME
// clicked/changed flags the mouse path sets - so button actions, settings
// bindings, and schematic UI events all work unchanged - and drives TextInput
// edit sessions (typed characters, caret movement, Enter commit / Escape
// cancel). Focus state lives in the UIContext; BuildVertices draws the ring
// and the caret from it.
#pragma once

#include "Core/Types.h"

namespace hbe {

class Scene;
class Input;

namespace ui {

struct UIContext;

// Advance focus/navigation/text editing against ctx.layout (last frame's
// rects, same as interaction). `dt` drives caret blink and stick auto-repeat.
void UpdateNavigation(Scene& scene, const Input& input, UIContext& ctx, f32 dt);

// True while a TextInput edit session is active. The engine feeds this into
// Input::SetTextCapture so gameplay/dev-menu keyboard reads go quiet while the
// user types.
bool WantsTextInput(const UIContext& ctx);

} // namespace ui
} // namespace hbe
