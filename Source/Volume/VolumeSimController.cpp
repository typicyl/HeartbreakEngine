// Source/Volume/VolumeSimController.cpp - see the header.
#include "Volume/VolumeSimController.h"

#include "Volume/IVolumeFrameSink.h"
#include "Volume/IVolumeSimSnapshot.h"
#include "Volume/VolumeSimRegistry.h"

#include <cstdio>

namespace hbe::volume {

VolumeSimController::VolumeSimController(IVolumeSimulation& sim, const VolumeSimConfig& config)
    : sim_(sim), config_(config), frameRate_(config.frameRate), substeps_(config.substeps) {
    if (frameRate_ <= 0.0f) frameRate_ = 30.0f;
    if (substeps_  <= 0)    substeps_  = 1;
    snapshotCapable_ = dynamic_cast<IVolumeSimSnapshot*>(&sim_) != nullptr;
}

void VolumeSimController::Reset() {
    sim_.Reset();
    frame_ = 0;
    keyframes_.clear();
    CaptureKeyframeIfDue();
}

void VolumeSimController::StepFrame() {
    const f32 dt = SubstepDt();
    for (int i = 0; i < substeps_; ++i) sim_.Step(dt);
    ++frame_;
    CaptureKeyframeIfDue();
}

void VolumeSimController::SeekFrame(u32 frame) {
    if (frame == frame_) return;
    if (frame < frame_) {
        // Scrub backward: restore the nearest earlier keyframe (or Reset), then re-step forward.
        if (!RestoreNearestKeyframe(frame)) Reset();
    }
    while (frame_ < frame) StepFrame();
}

void VolumeSimController::CaptureKeyframeIfDue() {
    if (!snapshotCapable_ || config_.keyframeInterval == 0) return;
    if (frame_ % config_.keyframeInterval != 0) return;
    for (const Keyframe& k : keyframes_)
        if (k.frame == frame_) return; // already captured this frame
    std::vector<u8> state;
    if (auto* snap = dynamic_cast<IVolumeSimSnapshot*>(&sim_); snap && snap->SaveState(state))
        keyframes_.push_back(Keyframe{frame_, std::move(state)});
}

bool VolumeSimController::RestoreNearestKeyframe(u32 targetFrame) {
    if (!snapshotCapable_) return false;
    const Keyframe* best = nullptr;
    for (const Keyframe& k : keyframes_)
        if (k.frame <= targetFrame && (!best || k.frame > best->frame)) best = &k;
    if (!best) return false;
    auto* snap = dynamic_cast<IVolumeSimSnapshot*>(&sim_);
    if (!snap || !snap->LoadState(best->state)) return false;
    frame_ = best->frame;
    return true;
}

void VolumeSimController::Record(IVolumeFrameSink& sink, u32 startFrame, u32 endFrame,
                                 const std::function<bool(u32, u32)>& onProgress) {
    if (endFrame < startFrame) return;
    const u32 frameCount = endFrame - startFrame + 1;
    sink.Begin(config_, frameCount, frameRate_);

    Reset();
    VolumeFrame scratch; // reused between frames; the sink copies what it needs
    for (u32 f = 0; f <= endFrame; ++f) {
        if (f >= startFrame) {
            sim_.ReadbackFrame(scratch);
            sink.Accept(f - startFrame, scratch);
            if (onProgress && onProgress(f - startFrame + 1, frameCount)) return; // aborted (partial)
        }
        if (f < endFrame) StepFrame();
    }
    sink.End();
}

// -------------------------------------------------------------------------------------------------
// --test-volsim self-test
// -------------------------------------------------------------------------------------------------
namespace {

f64 DensitySum(const VolumeFrame& fr) {
    const VolumeField* d = fr.field("density");
    if (!d) return 0.0;
    f64 s = 0.0;
    for (f32 v : d->data) s += static_cast<f64>(v);
    return s;
}

// Captures per-frame time + density sum, so the harness can assert timing + determinism.
struct RecordingSink final : IVolumeFrameSink {
    struct Entry { f32 time; f64 densitySum; };
    std::vector<Entry> entries;
    u32                begins = 0, ends = 0;

    void Begin(const VolumeSimConfig&, u32, f32) override { ++begins; entries.clear(); }
    void Accept(u32, const VolumeFrame& frame) override {
        entries.push_back(Entry{frame.time, DensitySum(frame)});
    }
    void End() override { ++ends; }
};

} // namespace

bool SelfTestVolumeController(std::string& report) {
    VolumeSimRegistry& reg = VolumeSimRegistry::Get();
    const VolumeSimTypeInfo* type = reg.Find("procedural-plume");
    if (!type) { report = "registry has no 'procedural-plume'"; return false; }

    VolumeSimConfig cfg = type->defaultConfig;
    cfg.frameRate = 30.0f;
    cfg.substeps  = 1;

    auto sim = reg.Create(cfg);
    if (!sim) { report = "registry failed to Create('procedural-plume')"; return false; }

    VolumeSimController ctrl(*sim, cfg);

    const u32 kFrames = 5;
    RecordingSink a;
    ctrl.Record(a, 0, kFrames - 1);

    if (a.begins != 1 || a.ends != 1) { report = "sink Begin/End not called exactly once"; return false; }
    if (a.entries.size() != kFrames) { report = "recorded frame count mismatch"; return false; }

    // Frame times must be i / frameRate, strictly increasing.
    for (u32 i = 0; i < kFrames; ++i) {
        const f32 expected = static_cast<f32>(i) / cfg.frameRate;
        if (std::abs(a.entries[i].time - expected) > 1e-4f) {
            report = "frame time mismatch at frame " + std::to_string(i);
            return false;
        }
    }
    // A plume must actually deposit density somewhere across the run.
    f64 total = 0.0;
    for (const auto& e : a.entries) total += e.densitySum;
    if (total <= 0.0) { report = "procedural plume produced zero density"; return false; }

    // Determinism: a second record must reproduce identical density sums bit-for-bit.
    auto sim2 = reg.Create(cfg);
    VolumeSimController ctrl2(*sim2, cfg);
    RecordingSink b;
    ctrl2.Record(b, 0, kFrames - 1);
    if (b.entries.size() != a.entries.size()) { report = "determinism: frame count changed"; return false; }
    for (u32 i = 0; i < kFrames; ++i) {
        if (a.entries[i].densitySum != b.entries[i].densitySum) {
            report = "determinism: density sum differs at frame " + std::to_string(i);
            return false;
        }
    }

    // Scrub: seeking to a frame must reproduce that frame's recorded state (re-sim from Reset).
    auto sim3 = reg.Create(cfg);
    VolumeSimController ctrl3(*sim3, cfg);
    ctrl3.Reset();
    ctrl3.SeekFrame(3);
    VolumeFrame seeked;
    ctrl3.ReadbackCurrent(seeked);
    if (DensitySum(seeked) != a.entries[3].densitySum) {
        report = "scrub: SeekFrame(3) did not reproduce recorded frame 3";
        return false;
    }
    // ...and seeking backward (3 -> 1) must also match.
    ctrl3.SeekFrame(1);
    ctrl3.ReadbackCurrent(seeked);
    if (DensitySum(seeked) != a.entries[1].densitySum) {
        report = "scrub: backward SeekFrame(1) did not reproduce recorded frame 1";
        return false;
    }

    report = "5 frames, deterministic, scrub OK (density total " + std::to_string(total) + ")";
    return true;
}

} // namespace hbe::volume
