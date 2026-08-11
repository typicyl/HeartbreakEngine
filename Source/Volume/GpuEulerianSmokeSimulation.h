// Source/Volume/GpuEulerianSmokeSimulation.h - the GPU-accelerated port of the CPU reference solver.
//
// Same collocated-grid Eulerian smoke sim as EulerianSmokeSimulation, but every per-voxel pass is a
// compute dispatch over STRUCTURED BUFFERS (velocity float4, scalars float4) run through the general
// RHI compute seam (CreateGpuBuffer/CreateComputePipeline/QueueCompute), the OceanFFT pattern. One
// Step() queues one substep's dispatch chain (10 + pressureIterations dispatches); the RHI drains it
// at the next BeginFrame with automatic inter-dispatch barriers. State lives in device-local buffers
// that persist across frames. ReadbackFrame ReadGpuBuffer's the current fields into a VolumeFrame -
// the SAME handoff the baker consumes, so this is a drop-in acceleration behind IVolumeSimulation.
//
// It is validated bit-close against the CPU oracle by --test-gpusolver. LIMITATION: interior solid
// obstacles are NOT modelled on the GPU (the domain is a closed box; walls via an in-kernel bounds
// test). A config with obstacles should use the CPU solver.
#pragma once

#include "RHI/RHI.h"
#include "Volume/IVolumeSimulation.h"
#include "Volume/VolumeFieldSet.h"
#include "Volume/VolumeSimConfig.h"

#include <glm/glm.hpp>

namespace hbe {
class Renderer;
}

namespace hbe::volume {

class GpuEulerianSmokeSimulation final : public IVolumeSimulation {
public:
    // Creates the buffers + pipelines up front (once). `renderer` must outlive this object.
    GpuEulerianSmokeSimulation(const VolumeSimConfig& config, Renderer& renderer);

    // Frees the device-local field buffers. The compute PIPELINES are intentionally NOT freed - the
    // RHI has no DestroyComputePipeline (same constraint as GpuOcean), so a per-instance solver
    // leaks its ~11 pipelines for the device's lifetime. Long-lived / shared-instance use is the
    // intended pattern for the editor; do not create-and-destroy one per frame.
    ~GpuEulerianSmokeSimulation() override;

    // True when GPU compute + all resources/pipelines are available. If false, the caller should
    // fall back to the CPU solver.
    bool Valid() const { return valid_; }

    void         Reset() override;
    void         Step(f32 dt) override;
    VolumeBounds GetBounds() const override { return bounds_; }
    FieldMask    AvailableFields() const override { return fields_.AvailableFields(); }
    void         ReadbackFrame(VolumeFrame& out) override;
    const char*  Name() const override { return "eulerian-smoke-gpu"; }

private:
    bool CreateResources();

    VolumeSimConfig config_;
    VolumeBounds    bounds_;
    glm::ivec3      dim_{0};
    glm::vec3       voxelSize_{1.0f};
    glm::vec3       invVoxel_{1.0f};
    f32             h_ = 1.0f;
    u32             count_ = 0;
    f32             time_ = 0.0f;
    VolumeFieldSet  fields_;
    Renderer*       renderer_ = nullptr;
    bool            valid_ = false;
    bool            needsInit_ = true;

    // Device-local structured-buffer fields (persist across frames).
    rhi::GpuBufferHandle velA_, velB_, sclA_, sclB_, pressA_, pressB_, div_, curl_;
    // The "current" state handles (swapped by the ping-pong passes each Step).
    rhi::GpuBufferHandle curVel_, otherVel_, curScl_, otherScl_;

    // One compute pipeline per pass (param-independent; created once, never destroyed).
    rhi::ComputePipelineHandle pInit_, pEmit_, pAdvectVel_, pBuoy_, pVort_, pConfine_, pDiv_, pClear_,
        pJacobi_, pGradSub_, pAdvectScl_;
};

} // namespace hbe::volume
