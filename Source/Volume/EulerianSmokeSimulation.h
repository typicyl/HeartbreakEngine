// Source/Volume/EulerianSmokeSimulation.h - the CPU REFERENCE fluid solver (the real thing).
//
// A genuine collocated-grid Eulerian smoke/fire simulation - density + temperature + velocity with
// semi-Lagrangian advection, Fedkiw buoyancy, vorticity confinement, grid-source emission, and a
// Jacobi pressure PROJECTION (the incompressibility solve is what makes it read as fluid rather than
// rising blobs). It is NOT particle splatting: emitters are source terms written into the grid.
//
// This is the golden ORACLE: deterministic, machine-independent (fixed dt, no atomics, gather-only
// per-voxel passes parallelized on the fiber job system), so a bake reproduces bit-for-bit and the
// later GPU solver is validated against it. It fills VolumeFrame and knows nothing about NanoVDB /
// the baker / the renderer. Implements IVolumeSimSnapshot so the controller can scrub quickly.
#pragma once

#include "Volume/IVolumeSimulation.h"
#include "Volume/IVolumeSimSnapshot.h"
#include "Volume/VolumeFieldSet.h"
#include "Volume/VolumeSimConfig.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::volume {

class EulerianSmokeSimulation final : public IVolumeSimulation, public IVolumeSimSnapshot {
public:
    explicit EulerianSmokeSimulation(const VolumeSimConfig& config);

    // IVolumeSimulation
    void         Reset() override;
    void         Step(f32 dt) override;
    VolumeBounds GetBounds() const override { return bounds_; }
    FieldMask    AvailableFields() const override { return fields_.AvailableFields(); }
    void         ReadbackFrame(VolumeFrame& out) override;
    const char*  Name() const override { return "eulerian-smoke"; }

    // IVolumeSimSnapshot (full-state save/restore for fast scrubbing).
    bool SaveState(std::vector<u8>& out) const override;
    bool LoadState(const std::vector<u8>& in) override;

private:
    // --- grid addressing -------------------------------------------------------------------------
    usize Idx(int x, int y, int z) const {
        return (static_cast<usize>(z) * dim_.y + y) * dim_.x + x;
    }
    bool InDomain(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < dim_.x && y < dim_.y && z < dim_.z;
    }
    // Out-of-domain (closed box) OR an obstacle cell counts as solid.
    bool Solid(int x, int y, int z) const {
        if (!InDomain(x, y, z)) return true;
        return solid_[Idx(x, y, z)] > 0.5f;
    }
    void Decode(usize i, int& x, int& y, int& z) const {
        const int plane = dim_.x * dim_.y;
        z = static_cast<int>(i) / plane;
        const int r = static_cast<int>(i) - z * plane;
        y = r / dim_.x;
        x = r - y * dim_.x;
    }

    // Manual trilinear sampling in GRID space (integer coords = cell centres), clamp-addressed.
    f32       SampleScalar(const std::vector<f32>& f, glm::vec3 g) const;
    glm::vec3 SampleVel(const std::vector<glm::vec3>& f, glm::vec3 g) const;

    // --- solver passes (one substep) -------------------------------------------------------------
    void EmitSources(f32 dt);
    void AdvectVelocity(f32 dt);
    void ApplyBuoyancy(f32 dt);
    void ComputeVorticity();
    void ApplyConfinement(f32 dt);
    void Project();
    void AdvectScalars(f32 dt);
    void EnforceSolids();

    // --- state -----------------------------------------------------------------------------------
    VolumeSimConfig config_;
    VolumeBounds    bounds_;
    glm::ivec3      dim_{0};
    glm::vec3       voxelSize_{1.0f};
    glm::vec3       invVoxel_{1.0f};
    f32             h_ = 1.0f;      // representative spacing (avg voxel size) for confinement
    usize           count_ = 0;
    f32             time_ = 0.0f;
    VolumeFieldSet  fields_;

    // Ping-pong + scratch fields (dense, row-major == VoxelIndex).
    std::vector<glm::vec3> vel_, velTmp_, curl_;
    std::vector<f32>       density_, densityTmp_;
    std::vector<f32>       temperature_, tempTmp_;
    std::vector<f32>       pressure_, pressureTmp_;
    std::vector<f32>       div_, curlMag_;
    std::vector<f32>       solid_; // static obstacle coverage (0 fluid .. 1 solid)
};

// --test-eulersim: run the solver headless on a small grid and assert it produces density, that a
// hot plume's centre of mass RISES (buoyancy + projection working), that no value is NaN/Inf, and
// that a re-run is bit-identical (determinism). Pure CPU. The Phase-1 correctness gate.
bool SelfTestEulerianSmoke(std::string& report);

} // namespace hbe::volume
