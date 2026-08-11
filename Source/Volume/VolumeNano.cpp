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

bool BuildScalarGridBlob(const VolumeField& field, const VolumeBounds& b,
                         std::vector<std::uint8_t>& outBytes, GridBuildStats& stats,
                         float threshold) {
    stats = GridBuildStats{};
    if (field.type != FieldType::Scalar) return false; // only scalar fields become float grids
    const std::size_t count = b.voxelCount();
    if (count == 0 || field.data.size() < count) return false;

    const glm::vec3 vs = b.voxelSize();
    if (vs.x <= 0.0f || vs.y <= 0.0f || vs.z <= 0.0f) return false;
    // NanoVDB's build::Grid transform here is a UNIFORM scale (setTransform takes one double), so only
    // near-cubic voxels map correctly. A non-cubic domain would silently bake wrong world geometry
    // into the shipped .hbvol (every off-axis sample reads the wrong world position) - refuse it.
    // (Anisotropic voxels would need a full nanovdb::Map; a documented follow-up.)
    const float vMax = std::fmax(vs.x, std::fmax(vs.y, vs.z));
    const float vMin = std::fmin(vs.x, std::fmin(vs.y, vs.z));
    if (vMax > vMin * 1.01f) return false;
    const double    scale = static_cast<double>(vs.x);        // cubic (guarded above)
    const glm::vec3 tr = b.worldMin + 0.5f * vs;              // index 0 -> centre of voxel 0
    const float     bg = field.background;

    nanovdb::tools::build::Grid<float> builder(bg, field.name.empty() ? "field" : field.name.c_str());
    builder.setTransform(scale, nanovdb::Vec3d(tr.x, tr.y, tr.z));
    auto acc = builder.getAccessor();
    bool any = false;
    for (int z = 0; z < b.dim.z; ++z)
        for (int y = 0; y < b.dim.y; ++y)
            for (int x = 0; x < b.dim.x; ++x) {
                const float v = field.data[VoxelIndex(b, x, y, z)];
                if (std::fabs(v - bg) <= threshold) continue; // prune to sparsity
                acc.setValue(nanovdb::Coord(x, y, z), v);
                ++stats.activeVoxels;
                if (!any) { stats.vmin = stats.vmax = v; any = true; }
                else { stats.vmin = std::fmin(stats.vmin, v); stats.vmax = std::fmax(stats.vmax, v); }
            }

    auto handle = nanovdb::tools::createNanoGrid(builder);
    const std::size_t bytes = static_cast<std::size_t>(handle.bufferSize());
    if (bytes == 0 || handle.data() == nullptr) return false;
    outBytes.resize(bytes);
    std::memcpy(outBytes.data(), handle.data(), bytes);
    return true;
}

bool BuildDensityGridBlob(const VolumeFrame& frame, std::vector<std::uint8_t>& outBytes,
                          glm::vec3& outMin, glm::vec3& outMax, float threshold) {
    const VolumeField* d = frame.field("density");
    if (d == nullptr || d->data.empty()) return false;
    GridBuildStats stats;
    if (!BuildScalarGridBlob(*d, frame.bounds, outBytes, stats, threshold)) return false;
    outMin = frame.bounds.worldMin;
    outMax = frame.bounds.worldMax;
    return true;
}

float SampleScalarGridBlob(const void* bytes, std::size_t byteSize, int x, int y, int z) {
    if (bytes == nullptr || byteSize < sizeof(nanovdb::GridData)) return 0.0f;
    // The blob is untrusted at runtime (streamed from a pack). Bound the grid to the buffer before
    // traversing its interior tree offsets, so a truncated/garbage blob cannot walk out of bounds.
    const auto* gd = static_cast<const nanovdb::GridData*>(bytes);
    if (gd->mGridSize < sizeof(nanovdb::GridData) || gd->mGridSize > byteSize) return 0.0f;
    const auto* grid = static_cast<const nanovdb::FloatGrid*>(bytes);
    if (grid->gridType() != nanovdb::GridType::Float) return 0.0f; // guards against non-float blobs
    return grid->getAccessor().getValue(nanovdb::Coord(x, y, z));
}

} // namespace hbe::volume
