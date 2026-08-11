// Source/Volume/VolumeSimController.h - the timeline layer above IVolumeSimulation.
//
// IVolumeSimulation stays minimal (Reset/Step/Readback). All the timeline semantics - fixed
// timestep, frame stepping, scrubbing (backward = re-simulate), and recording into a sink - live
// here, so EVERY solver gets scrub/record/replay for free. Fixed dt (frameRate x substeps, fixed
// COUNT) is mandatory for reproducible scrub + bake, mirroring the movie renderer's SetRenderFixedDt.
//
// Scrub-backward re-simulates from Reset() by default (always correct because the sim is
// deterministic). If the sim ALSO implements IVolumeSimSnapshot and config.keyframeInterval > 0, the
// controller caches full state at that cadence and restores the nearest keyframe to cut the re-sim.
#pragma once

#include "Volume/IVolumeSimulation.h"
#include "Volume/VolumeSimConfig.h"

#include <functional>
#include <string>
#include <vector>

namespace hbe::volume {

class IVolumeFrameSink;

class VolumeSimController {
public:
    // Borrows both `sim` and `config` (they must outlive the controller). Timing is read from config.
    VolumeSimController(IVolumeSimulation& sim, const VolumeSimConfig& config);

    void Reset();                 // sim.Reset(); cursor -> frame 0; clears cached keyframes
    void StepFrame();             // advance ONE baked frame (substeps x Step(SubstepDt))
    void SeekFrame(u32 frame);    // scrub to a frame (steps forward, or restores keyframe + re-steps)
    void ReadbackCurrent(VolumeFrame& out) { sim_.ReadbackFrame(out); }

    u32 CurrentFrame() const { return frame_; }
    f32 FrameDt()   const { return frameRate_ > 0.0f ? 1.0f / frameRate_ : 0.0f; }
    f32 SubstepDt() const { const f32 d = FrameDt(); return substeps_ > 0 ? d / static_cast<f32>(substeps_) : d; }
    f32 CurrentTime() const { return static_cast<f32>(frame_) * FrameDt(); }

    // Record frames [startFrame, endFrame] (inclusive) into `sink`. Resets, warms up to startFrame,
    // then captures each frame. Offline/headless - blocking readback per frame is fine here.
    // `onProgress(framesDone, framesTotal)` (optional) is called after each captured frame; returning
    // true ABORTS the record (partial - the sink is left incomplete) so a UI can cancel + show a bar.
    void Record(IVolumeFrameSink& sink, u32 startFrame, u32 endFrame,
                const std::function<bool(u32, u32)>& onProgress = {});

private:
    void CaptureKeyframeIfDue();  // snapshot full state at config.keyframeInterval cadence (opt-in)
    bool RestoreNearestKeyframe(u32 targetFrame); // -> true if a keyframe <= target was restored

    IVolumeSimulation&     sim_;
    const VolumeSimConfig& config_;
    f32                    frameRate_;
    int                    substeps_;
    u32                    frame_ = 0;

    // Opt-in snapshot acceleration (only if the sim implements IVolumeSimSnapshot).
    struct Keyframe { u32 frame; std::vector<u8> state; };
    std::vector<Keyframe>  keyframes_;
    bool                   snapshotCapable_ = false;
};

// Phase-0 gate: drive the built-in procedural-plume sim through the controller into a counting sink,
// asserting frame count, monotonically increasing frame time, deterministic re-record, and that
// SeekFrame reproduces a recorded frame. Pure CPU (no GPU/window). Exposed via `--test-volsim`.
bool SelfTestVolumeController(std::string& report);

} // namespace hbe::volume
