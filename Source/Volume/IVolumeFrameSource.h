// Source/Volume/IVolumeFrameSource.h - a RANDOM-ACCESS frame provider.
//
// The other side of playback: where IVolumeSimulation is CAUSAL (frame N needs N-1), a frame
// SOURCE offers O(1) access to any frame - an in-memory recorded ring for editor preview, a loaded
// .hbvol via VolumeCache at runtime, or an imported .vdb sequence. VolumeSimController plays back
// through either a live IVolumeSimulation (re-simulate) or an IVolumeFrameSource (seek), behind one
// timeline. Keeping this separate from IVolumeSimulation is what lets a baked/imported "solver"
// expose fast scrubbing without pretending to integrate anything.
#pragma once

#include "Volume/VolumeFrame.h"

namespace hbe::volume {

class IVolumeFrameSource {
public:
    virtual ~IVolumeFrameSource() = default;

    // Number of frames available (>= 1).
    virtual u32 FrameCount() const = 0;

    // Fill `out` with frame `index` (clamped to [0, FrameCount-1]). O(1) - no simulation.
    virtual void GetFrame(u32 index, VolumeFrame& out) = 0;

    // The world region + resolution the frames cover (usually constant across the sequence).
    virtual VolumeBounds GetBounds() const = 0;

    // Playback rate the sequence was recorded at (frames/sec); 0 if unknown/unused.
    virtual f32 FrameRate() const { return 0.0f; }
};

} // namespace hbe::volume
