// Editor/TimelineSnap.h - ONE definition of "which frame is this time on?".
//
// Every timeline in this editor stores time as float SECONDS and converted the mouse
// straight into it: `(x - originX) / pixelsPerSecond`, written into a key. Nothing
// rounded, nothing quantized, and there was no frame concept anywhere in the engine -
// so a hand-placed key landed on 1.3871429s and a second key intended to match it
// landed on 1.3866667s. The music editor was the sharpest case: it DREW a bar/beat
// grid it did not obey.
//
// This header is the shared, pure core so the three timelines (the cutscene NLE, the
// AnimationTrack strip, the music arrangement view) cannot drift from one another. It
// deliberately mirrors Editor/SaveDispatch.h: no ImGui, no Editor state, no
// filesystem - just Core/Types.h - which is what lets --test-timelinesnap prove it
// headlessly.
//
// AUTHORING ONLY. The runtime is still float-seconds end to end and is completely
// unaffected: snapping decides where a key is PLACED, never how it is SAMPLED.
#pragma once

#include "Core/Types.h"

namespace hbe::editor {

struct FrameGrid {
    f32 fps = 30.0f;     // ProjectSettings::timelineFps, or an asset's override
    bool enabled = true; // the panel's snap toggle
    // Ctrl held THIS FRAME: place between frames. Read ONCE per panel per frame into
    // this field, never per call site - otherwise a Ctrl press mid-drag can snap the
    // key and not the playhead, and they disagree by half a frame.
    bool suspend = false;
};

inline bool GridActive(const FrameGrid& g) {
    return g.enabled && !g.suspend && g.fps > 0.0f;
}

// Nearest frame index. Rounds half AWAY FROM ZERO (std::lround); every timeline
// clamps t >= 0 before snapping, so in practice that is round-half-up: at 30 fps,
// t = 0.016666... (exactly half a frame) lands on frame 1.
i32 TimeToFrame(f32 t, f32 fps);
f32 FrameToTime(i32 f, f32 fps);
// The last frame boundary at or before `duration`. A duration is rarely on a frame
// boundary, so clamping to `duration` itself would place a key off-grid at the very
// end of the timeline - the one spot an author is most likely to use.
f32 LastFrameTime(f32 duration, f32 fps);

// THE function. Returns `t` unchanged when the grid is off. IDEMPOTENT: Snap(Snap(t))
// == Snap(t) bit-exactly, which matters because a key's time is re-snapped on every
// drag frame, again on inspector release, and again on save.
f32 Snap(f32 t, const FrameGrid& g);

// Snap, THEN clamp into [0, LastFrameTime(duration)]. Use this for anything written
// into a key or the playhead - plain Snap can round the last frame PAST the duration.
f32 SnapClamped(f32 t, f32 duration, const FrameGrid& g);

// --test-timelinesnap: idempotence across every preset fps over the whole authorable
// range, the round-half-away-from-zero rule, the off-grid-duration clamp trap, and
// that a disabled or Ctrl-suspended grid is exactly the identity. Headless.
bool TimelineSnapSelfTest();

} // namespace hbe::editor
