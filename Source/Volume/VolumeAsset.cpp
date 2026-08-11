// Source/Volume/VolumeAsset.cpp - see the header.
#include "Volume/VolumeAsset.h"

#include "Volume/VolumeFormat.h"

#include <cstring>
#include <fstream>

namespace hbe::volume {

bool VolumeAsset::Load(const std::uint8_t* data, std::size_t size) {
    ok_ = false;
    frames_.clear();
    fieldNames_.clear();
    fieldTypes_.clear();
    fieldGridTypes_.clear();
    if (data == nullptr || size == 0) return false;
    bytes_.assign(data, data + size); // own a copy so GridViews stay valid

    ByteReader r(bytes_.data(), bytes_.size());
    const u8* magic = r.take(sizeof(kHbvolMagic));
    if (!magic || std::memcmp(magic, kHbvolMagic, sizeof(kHbvolMagic)) != 0) return false;
    version_ = r.u32v();
    if (version_ != kHbvolVersion) return false;
    flags_ = r.u32v();
    const u32 frameCount = r.u32v();
    fps_ = r.f32v();
    codec_ = r.u32v();
    const u32 fieldCount = r.u32v();
    // Braced-init evaluates its elements left-to-right (guaranteed), so the stateful reads are ordered.
    bounds_.worldMin = glm::vec3{r.f32v(), r.f32v(), r.f32v()};
    bounds_.worldMax = glm::vec3{r.f32v(), r.f32v(), r.f32v()};
    bounds_.dim = glm::ivec3{r.i32v(), r.i32v(), r.i32v()};
    sourceHash_ = r.u64v();
    // `.hbvol` is runtime-loaded (from packs / the VFS), so treat the header as untrusted: reject
    // implausible counts BEFORE any resize, or a malicious frameCount/fieldCount is an allocation bomb
    // (the per-element bounds checks below only fire once the loop starts reading).
    if (!r.ok || fieldCount == 0 || fieldCount > 64 || frameCount > 1000000u) return false;

    for (u32 i = 0; i < fieldCount; ++i) {
        fieldNames_.push_back(r.str());
        fieldTypes_.push_back(r.u32v());
        fieldGridTypes_.push_back(r.u32v());
    }
    if (!r.ok) return false;
    // Reject a frameCount the file cannot actually contain BEFORE resizing: each frame's index entry
    // is 4 (time) + fieldCount*28 (offset/size/active/min/max) bytes. Without this, a tiny crafted
    // header claiming a huge frameCount is an allocation bomb - the per-element bounds checks in the
    // loop below only fire AFTER frames_.resize() + fieldCount resizes have already committed the RAM.
    const u64 perFrame = 4ull + static_cast<u64>(fieldCount) * 28ull;
    if (static_cast<u64>(frameCount) * perFrame > static_cast<u64>(r.end - r.p)) return false;
    frames_.resize(frameCount);
    for (u32 fi = 0; fi < frameCount; ++fi) {
        frames_[fi].time = r.f32v();
        frames_[fi].slots.resize(fieldCount);
        for (u32 s = 0; s < fieldCount; ++s) {
            Slot& sl = frames_[fi].slots[s];
            sl.off = r.u64v();
            sl.size = r.u64v();
            sl.active = r.u32v();
            sl.vmin = r.f32v();
            sl.vmax = r.f32v();
        }
    }
    if (!r.ok) return false;
    payloadOffset_ = r.consumed(bytes_.data());

    // Every non-empty slot must lie inside the payload section. Written to avoid u64 overflow on a
    // crafted (off, size) pair (off + size could wrap past the buffer end).
    const u64 payloadBytes = static_cast<u64>(bytes_.size() - payloadOffset_);
    for (const FrameIdx& f : frames_)
        for (const Slot& s : f.slots)
            if (s.size > 0 && (s.off > payloadBytes || s.size > payloadBytes - s.off)) return false;

    ok_ = true;
    return true;
}

bool VolumeAsset::LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    if (!f.read(reinterpret_cast<char*>(buf.data()), n)) return false;
    return Load(buf.data(), buf.size());
}

int VolumeAsset::FieldIndex(const std::string& name) const {
    for (std::size_t i = 0; i < fieldNames_.size(); ++i)
        if (fieldNames_[i] == name) return static_cast<int>(i);
    return -1;
}

f32 VolumeAsset::FrameTime(u32 frame) const {
    return frame < frames_.size() ? frames_[frame].time : 0.0f;
}

VolumeAsset::GridView VolumeAsset::Grid(u32 frame, u32 fieldIndex) const {
    GridView v;
    if (!ok_ || frame >= frames_.size() || fieldIndex >= frames_[frame].slots.size()) return v;
    const Slot& s = frames_[frame].slots[fieldIndex];
    if (s.size == 0) return v; // all-background this frame
    v.bytes = bytes_.data() + payloadOffset_ + s.off;
    v.size = static_cast<std::size_t>(s.size);
    v.activeVoxels = s.active;
    v.vmin = s.vmin;
    v.vmax = s.vmax;
    return v;
}

VolumeAsset::GridView VolumeAsset::Grid(u32 frame, const std::string& fieldName) const {
    const int idx = FieldIndex(fieldName);
    return idx < 0 ? GridView{} : Grid(frame, static_cast<u32>(idx));
}

bool ReadHbvolSourceHash(const std::string& path, u64& outHash) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    // Header prefix: magic[8] + version/flags/frameCount/fps/codec/fieldCount (6*4) + worldMin[3] +
    // worldMax[3] + dim[3] (3*12) + sourceHash(8). sourceHash sits at offset 68.
    u8 hdr[76];
    if (!f.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) return false;
    if (std::memcmp(hdr, kHbvolMagic, sizeof(kHbvolMagic)) != 0) return false;
    std::memcpy(&outHash, hdr + 68, sizeof(u64));
    return true;
}

} // namespace hbe::volume
