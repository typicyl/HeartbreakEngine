// Source/Volume/ProceduralVolumeSimulation.h - a self-contained, deterministic animated smoke
// plume as an IVolumeSimulation. NO GPU, NO scene, NO splat: purely a function of voxel position
// + accumulated time, so Reset()+Step() to the same time reproduces the frame exactly. This is
// the SOURCE-INDEPENDENT source used to develop + test the bake/runtime pipeline and to prove the
// VolumeBaker never depends on the legacy splat (see --test-hbvol).
#pragma once

#include "Volume/IVolumeSimulation.h"

namespace hbe::volume {

class ProceduralVolumeSimulation final : public IVolumeSimulation {
public:
    explicit ProceduralVolumeSimulation(glm::ivec3 dim = glm::ivec3(40, 80, 40));

    void Reset() override;
    void Step(f32 dt) override;
    VolumeBounds GetBounds() const override { return bounds_; }
    FieldMask AvailableFields() const override {
        return FieldMask::Density | FieldMask::Temperature;
    }
    void ReadbackFrame(VolumeFrame& out) override;
    const char* Name() const override { return "procedural-plume"; }

private:
    VolumeBounds bounds_;
    f32 time_ = 0.0f;
};

} // namespace hbe::volume
