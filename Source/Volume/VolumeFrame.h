// Source/Volume/VolumeFrame.h - the SINGLE interchange type between any volume simulation /
// authoring source and the VolumeBaker. This is the heart of the simulation-agnostic design:
// the baker consumes only VolumeFrame and never knows what produced it (the legacy splat, a
// future OpenVDB sim, an imported .vdb sequence, a procedural generator, ...).
//
// Fields are named + generic (density/temperature now; velocity/fuel/flame/vorticity/custom
// later) so adding a field never touches the baker, the .hbvol format, or the runtime. Data is
// DENSE here (simple for a CPU source); the baker sparsifies it into NanoVDB. Nothing NanoVDB /
// splat / particle specific appears in this header.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hbe::volume {

enum class FieldType : u8 { Scalar = 1, Vector3 = 3 }; // value == component count

// Standard field identities (a source advertises which it produces). Extensible: the pipeline
// keys off the field NAME string, not this enum - this is just a cheap capability summary.
enum class FieldMask : u32 {
    None        = 0,
    Density     = 1u << 0,
    Temperature = 1u << 1,
    Velocity    = 1u << 2, // future
    Fuel        = 1u << 3, // future
    Flame       = 1u << 4, // future
    Vorticity   = 1u << 5, // future
};
inline FieldMask operator|(FieldMask a, FieldMask b) {
    return static_cast<FieldMask>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline FieldMask operator&(FieldMask a, FieldMask b) {
    return static_cast<FieldMask>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline bool Any(FieldMask m) { return static_cast<u32>(m) != 0u; }

// The world-space region a frame covers + its voxel resolution. index<->world is a plain linear
// map (voxel i spans [worldMin + i*voxelSize, worldMin + (i+1)*voxelSize]); the baker hands this
// to NanoVDB as the grid transform so runtime world->index sampling matches.
struct VolumeBounds {
    glm::vec3  worldMin{0.0f};
    glm::vec3  worldMax{1.0f};
    glm::ivec3 dim{0, 0, 0}; // voxel counts per axis

    glm::vec3 extent() const { return worldMax - worldMin; }
    glm::vec3 voxelSize() const {
        const glm::vec3 e = extent();
        return glm::vec3(dim.x > 0 ? e.x / static_cast<f32>(dim.x) : 0.0f,
                         dim.y > 0 ? e.y / static_cast<f32>(dim.y) : 0.0f,
                         dim.z > 0 ? e.z / static_cast<f32>(dim.z) : 0.0f);
    }
    usize voxelCount() const {
        return static_cast<usize>(glm::max(dim.x, 0)) * static_cast<usize>(glm::max(dim.y, 0)) *
               static_cast<usize>(glm::max(dim.z, 0));
    }
    // World centre of voxel (x,y,z).
    glm::vec3 voxelCenter(int x, int y, int z) const {
        const glm::vec3 vs = voxelSize();
        return worldMin + glm::vec3((x + 0.5f) * vs.x, (y + 0.5f) * vs.y, (z + 0.5f) * vs.z);
    }
};

// One named, dense field over the frame's voxel grid. `data` holds voxelCount * components()
// floats, row-major with X fastest then Y then Z: idx = (z*dim.y + y)*dim.x + x, times components.
struct VolumeField {
    std::string        name;                    // interchange key: "density", "temperature", ...
    FieldType          type = FieldType::Scalar;
    f32                background = 0.0f;        // "empty" value the baker prunes to sparsity
    std::vector<f32>   data;

    int components() const { return static_cast<int>(type); }
};

// A single captured frame of volume state - fields + bounds + time. The one type the baker eats.
struct VolumeFrame {
    f32                     time = 0.0f;
    VolumeBounds            bounds;
    std::vector<VolumeField> fields;

    const VolumeField* field(std::string_view name) const {
        for (const auto& f : fields)
            if (f.name == name) return &f;
        return nullptr;
    }
    VolumeField* field(std::string_view name) {
        for (auto& f : fields)
            if (f.name == name) return &f;
        return nullptr;
    }
    // Get-or-create a field sized to the current bounds (zeroed to `background`).
    VolumeField& ensureField(std::string_view name, FieldType type, f32 background = 0.0f) {
        if (VolumeField* existing = field(name)) return *existing;
        VolumeField f;
        f.name = std::string(name);
        f.type = type;
        f.background = background;
        f.data.assign(bounds.voxelCount() * static_cast<usize>(static_cast<int>(type)), background);
        fields.push_back(std::move(f));
        return fields.back();
    }
};

// Row-major flat index of voxel (x,y,z) for the scalar case (multiply by components for vectors).
inline usize VoxelIndex(const VolumeBounds& b, int x, int y, int z) {
    return (static_cast<usize>(z) * static_cast<usize>(b.dim.y) + static_cast<usize>(y)) *
               static_cast<usize>(b.dim.x) +
           static_cast<usize>(x);
}

} // namespace hbe::volume
