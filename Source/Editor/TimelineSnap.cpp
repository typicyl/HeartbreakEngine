// Editor/TimelineSnap.cpp - the frame grid, and its proof.
#include "Editor/TimelineSnap.h"

#include <cmath>
#include <cstdio>

namespace hbe::editor {

i32 TimeToFrame(f32 t, f32 fps) {
    // The `!(fps > 0)` form (rather than `fps <= 0`) also rejects NaN, which reaches
    // here from a corrupt project file before the clamp on load can run.
    if (!(fps > 0.0f) || !std::isfinite(t)) return 0;
    return static_cast<i32>(std::lround(static_cast<double>(t) * static_cast<double>(fps)));
}

f32 FrameToTime(i32 f, f32 fps) {
    return fps > 0.0f ? static_cast<f32>(static_cast<double>(f) / static_cast<double>(fps))
                      : 0.0f;
}

f32 LastFrameTime(f32 duration, f32 fps) {
    if (!(fps > 0.0f) || !(duration > 0.0f)) return 0.0f;
    return static_cast<f32>(
        std::floor(static_cast<double>(duration) * static_cast<double>(fps)) /
        static_cast<double>(fps));
}

f32 Snap(f32 t, const FrameGrid& g) {
    if (!GridActive(g) || !std::isfinite(t)) return t;
    // The t*fps intermediate is computed in DOUBLE. With f32 throughout, n/fps rounds
    // to the nearest f32 and (f32)(n/fps)*fps can land at n +/- 1e-7*n - still under
    // half a frame for any authorable time, but the double intermediate makes
    // idempotence trivially true instead of an argument that has to be re-checked.
    return FrameToTime(TimeToFrame(t, g.fps), g.fps);
}

f32 SnapClamped(f32 t, f32 duration, const FrameGrid& g) {
    const f32 s = Snap(t, g);
    const f32 hi = GridActive(g) ? LastFrameTime(duration, g.fps)
                                 : (duration > 0.0f ? duration : 0.0f);
    const f32 top = hi > 0.0f ? hi : 0.0f;
    if (!std::isfinite(s)) return 0.0f;
    return s < 0.0f ? 0.0f : (s > top ? top : s);
}

// --- `--test-timelinesnap` ---------------------------------------------------

namespace {
int g_fails = 0;

void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("timelinesnap FAIL: %s\n", what);
}

constexpr f32 kPresetFps[] = {23.976f, 24.0f, 25.0f, 30.0f, 48.0f, 50.0f, 60.0f, 120.0f};
} // namespace

bool TimelineSnapSelfTest() {
    g_fails = 0;

    // 1) IDEMPOTENCE over the whole authorable range at every preset rate. This is
    //    the load-bearing property: a key's time is re-snapped on every drag frame,
    //    on inspector release, and again on save, so a Snap that moved an
    //    already-snapped value would drift a key a little further every gesture.
    for (const f32 fps : kPresetFps) {
        const FrameGrid g{fps, true, false};
        bool ok = true;
        for (int i = 0; i < 200000 && ok; ++i) {
            const f32 t = static_cast<f32>(i) * 0.018f; // 0 .. 3600 s
            const f32 a = Snap(t, g);
            if (Snap(a, g) != a) ok = false;
        }
        Check(ok, "Snap is not idempotent at some preset frame rate");
    }

    // 2) A SNAPPED TIME IS EXACTLY A FRAME BOUNDARY, and lands within half a frame of
    //    where the author released the mouse.
    for (const f32 fps : kPresetFps) {
        const FrameGrid g{fps, true, false};
        const f32 half = 0.5f / fps;
        bool onGrid = true, near = true;
        for (int i = 0; i < 5000; ++i) {
            const f32 t = static_cast<f32>(i) * 0.0137f;
            const f32 s = Snap(t, g);
            if (s != FrameToTime(TimeToFrame(s, fps), fps)) onGrid = false;
            // A tolerance is required: `half` is itself a rounded f32.
            if (std::fabs(s - t) > half + 1e-4f) near = false;
        }
        Check(onGrid, "a snapped time is not on a frame boundary");
        Check(near, "a snapped time moved by more than half a frame");
    }

    // 3) THE ROUNDING RULE, pinned. Exactly half a frame rounds UP (lround rounds
    //    half away from zero, and every timeline clamps t >= 0 first).
    {
        const FrameGrid g{30.0f, true, false};
        Check(TimeToFrame(0.5f / 30.0f, 30.0f) == 1, "half a frame must round up");
        Check(TimeToFrame(0.49f / 30.0f, 30.0f) == 0, "just under half a frame must round down");
        Check(Snap(0.0f, g) == 0.0f, "zero must stay zero");
        Check(Snap(1.0f, g) == 1.0f, "an exact second is already on the 30 fps grid");
    }

    // 4) THE CLAMP TRAP: a duration that is NOT on a frame boundary. Snapping alone
    //    can round the final key PAST the end of the timeline, which then reads as a
    //    key that never fires.
    {
        const FrameGrid g{30.0f, true, false};
        const f32 dur = 5.02f; // 150.6 frames - deliberately off-grid
        Check(SnapClamped(5.019f, dur, g) <= dur,
              "SnapClamped returned a time past the duration");
        Check(SnapClamped(1e6f, dur, g) <= dur, "a far-past-the-end drag was not clamped");
        Check(SnapClamped(-50.0f, dur, g) == 0.0f, "a negative time was not clamped to 0");
        Check(LastFrameTime(dur, 30.0f) <= dur, "LastFrameTime exceeded the duration");
        Check(LastFrameTime(dur, 30.0f) == 5.0f, "LastFrameTime should floor 150.6 -> frame 150");
    }

    // 5) A DISABLED OR SUSPENDED GRID IS EXACTLY THE IDENTITY. This is what makes
    //    "hold Ctrl to place between frames" trustworthy - it must not quietly round.
    {
        const f32 odd = 1.3871429f;
        Check(Snap(odd, {30.0f, false, false}) == odd, "a disabled grid must not move a time");
        Check(Snap(odd, {30.0f, true, true}) == odd, "a Ctrl-suspended grid must not move a time");
        Check(Snap(odd, {0.0f, true, false}) == odd, "a zero frame rate must not move a time");
        // ...and the un-snapped path still clamps to the real duration, not to a frame.
        Check(SnapClamped(9.9f, 5.02f, {30.0f, false, false}) == 5.02f,
              "an unsnapped clamp must use the duration itself");
    }

    // 6) DEGENERATE INPUTS do not produce a NaN or a wild value in a key.
    {
        const FrameGrid g{30.0f, true, false};
        const f32 nan = std::nanf("");
        Check(std::isfinite(SnapClamped(nan, 5.0f, g)), "SnapClamped let a NaN through");
        Check(TimeToFrame(nan, 30.0f) == 0, "TimeToFrame let a NaN through");
        Check(TimeToFrame(1.0f, 0.0f) == 0, "a zero frame rate must not divide");
        Check(LastFrameTime(5.0f, 0.0f) == 0.0f, "LastFrameTime must reject a zero frame rate");
        Check(SnapClamped(1.0f, 0.0f, g) == 0.0f, "a zero-duration timeline must clamp to 0");
    }

    if (g_fails == 0) {
        std::printf("timelinesnap: %zu frame rates, idempotent over 0-3600s; half a frame "
                    "rounds up; snapped keys never pass the duration; a disabled or "
                    "Ctrl-suspended grid is the identity\n",
                    sizeof(kPresetFps) / sizeof(kPresetFps[0]));
    }
    return g_fails == 0;
}

} // namespace hbe::editor
