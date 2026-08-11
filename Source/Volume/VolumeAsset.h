// Source/Volume/VolumeAsset.h - a loaded `.hbvol` baked volume cache (reader side).
//
// Parses the header + per-frame/per-field seek index (VolumeFormat.h) and owns the byte buffer, so a
// GridView borrows directly into it with no copy - what VolumeCache/VolumeComponent stream and hand to
// the runtime PNanoVDB raymarch. NanoVDB-free: it only carries raw grid blobs.
#pragma once

#include "Volume/VolumeFrame.h" // VolumeBounds

#include <cstdint>
#include <string>
#include <vector>

namespace hbe::volume {

class VolumeAsset {
public:
    bool Load(const std::uint8_t* data, std::size_t size);
    bool Load(const std::vector<std::uint8_t>& bytes) { return Load(bytes.data(), bytes.size()); }
    bool LoadFile(const std::string& path);

    bool                            Ok() const { return ok_; }
    u32                             FrameCount() const { return static_cast<u32>(frames_.size()); }
    f32                             Fps() const { return fps_; }
    VolumeBounds                    Bounds() const { return bounds_; }
    u64                             SourceHash() const { return sourceHash_; }
    const std::vector<std::string>& FieldNames() const { return fieldNames_; }
    int                             FieldIndex(const std::string& name) const;
    f32                             FrameTime(u32 frame) const;

    // A borrowed view of one frame/field's raw NanoVDB grid blob (empty when the field is all
    // background that frame, or on out-of-range).
    struct GridView {
        const std::uint8_t* bytes = nullptr;
        std::size_t         size = 0;
        u32                 activeVoxels = 0;
        f32                 vmin = 0.0f, vmax = 0.0f;
        bool                valid() const { return bytes != nullptr && size != 0; }
    };
    GridView Grid(u32 frame, u32 fieldIndex) const;
    GridView Grid(u32 frame, const std::string& fieldName) const;

private:
    std::vector<std::uint8_t> bytes_; // owned copy; GridViews point into it
    bool                      ok_ = false;
    u32                       version_ = 0, flags_ = 0, codec_ = 0;
    f32                       fps_ = 30.0f;
    u64                       sourceHash_ = 0;
    VolumeBounds              bounds_;
    std::vector<std::string>  fieldNames_;
    std::vector<u32>          fieldTypes_, fieldGridTypes_;
    struct Slot { u64 off = 0, size = 0; u32 active = 0; f32 vmin = 0, vmax = 0; };
    struct FrameIdx { f32 time = 0; std::vector<Slot> slots; };
    std::vector<FrameIdx>     frames_;
    std::size_t               payloadOffset_ = 0;
};

// Cheap header-only read of a `.hbvol`'s stamped sourceHash (for stale-bake detection) - reads just
// the fixed header prefix, not the whole file. Returns false if the file is missing / not a `.hbvol`.
bool ReadHbvolSourceHash(const std::string& path, u64& outHash);

} // namespace hbe::volume
