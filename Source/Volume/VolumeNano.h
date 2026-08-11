// Source/Volume/VolumeNano.h - NanoVDB self-test + (later) the CPU-side NanoVDB helpers.
//
// This is the ONLY place NanoVDB C++ headers are pulled in on the CPU side; the rest of the
// Volume subsystem (VolumeBaker/VolumeAsset/VolumeCache) talks to NanoVDB through here so
// NanoVDB types never leak into the RHI or the renderer. NanoVDB is header-only with zero
// external dependencies (all NANOVDB_USE_* off); OpenVDB is a later optional editor-only dep.
#pragma once

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

} // namespace hbe::volume
