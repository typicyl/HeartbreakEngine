// Source/Volume/VolumeNano.h - NanoVDB self-test + (later) the CPU-side NanoVDB helpers.
//
// This is the ONLY place NanoVDB C++ headers are pulled in on the CPU side; the rest of the
// Volume subsystem (VolumeBaker/VolumeAsset/VolumeCache) talks to NanoVDB through here so
// NanoVDB types never leak into the RHI or the renderer. NanoVDB is header-only with zero
// external dependencies (all NANOVDB_USE_* off); OpenVDB is a later optional editor-only dep.
#pragma once

#include "Volume/VolumeFrame.h"

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hbe::volume {

// P0 gate: build a tiny sparse grid, convert it to a NanoVDB grid, and read a value back -
// proving the header-only NanoVDB build (grid builder + createNanoGrid + accessor) compiles
// and links with no external deps. Returns true on success; `report` gets a readable line
// either way. Exposed via `HeartbreakEditor --test-nanovdb`.
bool SelfTestNanoVDB(std::string& report);

// Build a static test volume - a soft density sphere - as a raw, byte-exact NanoVDB grid blob
// (float grid), for the P1 `--volume-preview` harness / early integration tests. The grid uses
// voxel size 1 with no translation, so index space == world space and the world AABB is
// [0,dim]^3 (returned in outMin/outMax). Returns false if the grid could not be built.
bool BuildTestVolumeBlob(std::vector<std::uint8_t>& outBytes, glm::vec3& outMin, glm::vec3& outMax,
                         int dim = 64);

// Bridge a simulated VolumeFrame's "density" field into a raw, byte-exact NanoVDB float grid blob
// carrying the frame's WORLD transform (voxel size + translation from VolumeBounds), so the runtime
// PNanoVDB raymarch samples it at the correct world position. Voxels at/below `threshold` are pruned
// to sparsity (background 0). outMin/outMax return the world AABB (== frame.bounds). This is a
// minimal seed of VolumeBaker (single field, no codec/.hbvol) used to EYEBALL the solver now; it
// assumes near-cubic voxels (uniform scale from voxelSize.x). Returns false if there is no density.
bool BuildDensityGridBlob(const VolumeFrame& frame, std::vector<std::uint8_t>& outBytes,
                          glm::vec3& outMin, glm::vec3& outMax, float threshold = 0.01f);

// Per-grid statistics the baker records in the .hbvol frame index.
struct GridBuildStats {
    std::uint32_t activeVoxels = 0;
    float         vmin = 0.0f;
    float         vmax = 0.0f;
};

// Build a byte-exact NanoVDB FLOAT grid blob from a SCALAR VolumeField, carrying the frame's world
// transform (voxel size + translation from `bounds`, assuming near-cubic voxels). `field.background`
// is the grid background; voxels within `threshold` of it are PRUNED to sparsity. This is the core
// the VolumeBaker uses per field per frame. Returns false if the field has no data.
bool BuildScalarGridBlob(const VolumeField& field, const VolumeBounds& bounds,
                         std::vector<std::uint8_t>& outBytes, GridBuildStats& stats,
                         float threshold = 1e-4f);

// Host-side sample of a NanoVDB FLOAT grid blob at integer voxel coords (index space). Returns the
// grid background for inactive voxels, or 0 if the blob is not a valid float grid. Used by
// --test-hbvol to verify the bake round-trips.
float SampleScalarGridBlob(const void* bytes, std::size_t byteSize, int x, int y, int z);

} // namespace hbe::volume
