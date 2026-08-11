// Source/Volume/VolumeNano.cpp - see VolumeNano.h.
#include "Volume/VolumeNano.h"

// Header-only NanoVDB (no external deps; all NANOVDB_USE_* off). These are the exact headers
// VolumeBaker will use: the grid builder (dense/sparse authoring) + the OpenVDB->/build->NanoVDB
// converter. Namespaces/paths pinned to OpenVDB v12 (nanovdb::tools::*).
#include <nanovdb/NanoVDB.h>
#include <nanovdb/tools/GridBuilder.h>
#include <nanovdb/tools/CreateNanoGrid.h>

#include <cmath>
#include <cstring>

namespace hbe::volume {

bool SelfTestNanoVDB(std::string& report) {
    // Build a tiny sparse float grid (background 0) with a couple of active voxels.
    nanovdb::tools::build::Grid<float> builder(0.0f, "density");
    auto acc = builder.getAccessor();
    acc.setValue(nanovdb::Coord(0, 0, 0), 1.0f);
    acc.setValue(nanovdb::Coord(3, 1, 2), 0.5f);

    // Convert to a NanoVDB grid on a host buffer - the exact path VolumeBaker will use.
    auto handle = nanovdb::tools::createNanoGrid(builder);
    const auto* grid = handle.template grid<float>();
    if (grid == nullptr) {
        report = "NanoVDB self-test FAILED: createNanoGrid produced no float grid";
        return false;
    }

    const float v = grid->getAccessor().getValue(nanovdb::Coord(0, 0, 0));
    const unsigned long long active = static_cast<unsigned long long>(grid->activeVoxelCount());
    const unsigned long long bytes = static_cast<unsigned long long>(handle.bufferSize());
    report = "NanoVDB OK: " + std::to_string(bytes) + " bytes, sample(0,0,0)=" +
             std::to_string(v) + ", activeVoxels=" + std::to_string(active);
    return v > 0.9f && bytes > 0 && active >= 2;
}

bool BuildTestVolumeBlob(std::vector<std::uint8_t>& outBytes, glm::vec3& outMin, glm::vec3& outMax,
                         int dim) {
    if (dim < 4) dim = 4;
    nanovdb::tools::build::Grid<float> builder(0.0f, "density");
    auto acc = builder.getAccessor();
    const float r = static_cast<float>(dim) * 0.32f;      // sphere radius (voxels)
    const float c = static_cast<float>(dim) * 0.5f;       // centre (voxels)
    for (int z = 0; z < dim; ++z)
        for (int y = 0; y < dim; ++y)
            for (int x = 0; x < dim; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - c;
                const float dy = static_cast<float>(y) + 0.5f - c;
                const float dz = static_cast<float>(z) + 0.5f - c;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float density = 1.0f - d / r;       // soft sphere, 1 at centre -> 0 at r
                if (density > 0.02f) acc.setValue(nanovdb::Coord(x, y, z), density);
            }
    auto handle = nanovdb::tools::createNanoGrid(builder);
    const std::size_t bytes = static_cast<std::size_t>(handle.bufferSize());
    if (bytes == 0 || handle.data() == nullptr) return false;
    outBytes.resize(bytes);
    std::memcpy(outBytes.data(), handle.data(), bytes);
    outMin = glm::vec3(0.0f);
    outMax = glm::vec3(static_cast<float>(dim)); // voxel size 1 -> index space == world space
    return true;
}

} // namespace hbe::volume
