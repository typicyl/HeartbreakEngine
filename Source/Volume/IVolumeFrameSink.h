// Source/Volume/IVolumeFrameSink.h - the RECORD output contract.
//
// A sink receives a sequence of baked VolumeFrames. The controller (VolumeSimController::Record)
// drives a simulation at fixed dt and hands each captured frame to a sink - it never names the
// concrete sink. The VolumeBaker IS a sink, but so is a debug frame-counter or an in-memory ring
// for editor scrubbing. This inversion is what keeps the whole simulation/framework layer free of
// any baker / NanoVDB / .hbvol dependency (the point of the redesign).
#pragma once

#include "Volume/VolumeFrame.h"

namespace hbe::volume {

struct VolumeSimConfig; // fwd (VolumeSimConfig.h) - passed to Begin for metadata only.

class IVolumeFrameSink {
public:
    virtual ~IVolumeFrameSink() = default;

    // Called once before any frame. `frameCount` is how many Accept() calls will follow;
    // `frameRate` is the bake rate (frames/sec). `config` is metadata (bounds, bake field list,
    // model id) the sink may record - the sink must NOT simulate anything from it.
    virtual void Begin(const VolumeSimConfig& config, u32 frameCount, f32 frameRate) = 0;

    // Called once per recorded frame, `frameIndex` in [0, frameCount). `frame` is a transient
    // buffer reused between calls - a sink that needs to keep data must copy it now.
    virtual void Accept(u32 frameIndex, const VolumeFrame& frame) = 0;

    // Called once after the last frame (finalize/flush).
    virtual void End() = 0;
};

} // namespace hbe::volume
