// Source/Volume/IVolumeSimSnapshot.h - OPT-IN full-state save/restore for a simulation.
//
// IVolumeSimulation guarantees "scrub = re-simulate from Reset()", which is always correct but
// O(frames). A solver that ALSO implements this interface lets VolumeSimController cache full
// internal state every keyframeInterval frames, so scrubbing backward restores the nearest
// keyframe and only re-steps the remainder. Kept OUT of the base interface so the minimal contract
// stays minimal: a solver that does not implement it is simply always re-simulated from Reset()
// (correct, just slower to scrub), and a cached IVolumeFrameSource never needs it at all.
#pragma once

#include "Core/Types.h"

#include <vector>

namespace hbe::volume {

class IVolumeSimSnapshot {
public:
    virtual ~IVolumeSimSnapshot() = default;

    // Serialize ALL authoritative sim state (every field + cursor) into `out`. Must be sufficient
    // to resume stepping bit-identically. Return false if snapshotting is unavailable this frame.
    virtual bool SaveState(std::vector<u8>& out) const = 0;

    // Restore state previously produced by SaveState on the SAME sim/config. Return false on any
    // mismatch (size/version); the controller then falls back to Reset()+re-step.
    virtual bool LoadState(const std::vector<u8>& in) = 0;
};

} // namespace hbe::volume
