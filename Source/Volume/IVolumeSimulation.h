// Source/Volume/IVolumeSimulation.h - the source-side contract of the volume pipeline.
//
// ANY volume source implements this: the legacy splat (via SplatVolumeSimulation), a procedural
// generator, a future OpenVDB / fluid solver, or an imported .vdb sequence. The VolumeBaker only
// ever sees this interface + the VolumeFrame it produces - it has NO knowledge of, or dependency
// on, the concrete source. That independence is the whole point of the redesign; keep it.
//
//   Reset()          -> initial (t=0) state
//   Step(dt)         -> advance the simulation by dt seconds
//   GetBounds()      -> the world AABB + voxel resolution of the current state
//   AvailableFields()-> which standard fields this source can produce
//   ReadbackFrame()  -> fill a VolumeFrame with the current state (the ONLY handoff to the baker)
#pragma once

#include "Volume/VolumeFrame.h"

namespace hbe::volume {

class IVolumeSimulation {
public:
    virtual ~IVolumeSimulation() = default;

    // Return to the deterministic initial state (so a Record run is reproducible / re-bakeable).
    virtual void Reset() = 0;

    // Advance the simulation by dt seconds. Sources that are inherently time-parametric (e.g.
    // procedural) may simply accumulate time; stateful solvers integrate.
    virtual void Step(f32 dt) = 0;

    // The world-space region + voxel resolution the current frame occupies.
    virtual VolumeBounds GetBounds() const = 0;

    // Which standard fields this source can fill (a capability summary; the baker still keys off
    // field names in the produced VolumeFrame).
    virtual FieldMask AvailableFields() const = 0;

    // Fill `out` with the current state (reusing/allocating out.fields + out.bounds + out.time).
    // This is the ONLY thing the baker consumes. Implementations must be deterministic given the
    // same Reset()+Step() sequence so a bake is reproducible.
    virtual void ReadbackFrame(VolumeFrame& out) = 0;

    virtual const char* Name() const { return "volume-sim"; }
};

} // namespace hbe::volume
