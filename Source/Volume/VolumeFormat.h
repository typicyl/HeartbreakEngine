// Source/Volume/VolumeFormat.h - the on-disk `.hbvol` baked-volume container + tiny POD (de)serializers.
//
// `.hbvol` is the native baked volume cache: a header, a per-frame/per-field seek index, and a payload
// of byte-exact NanoVDB grid blobs (one grid per baked field per frame). It is SOURCE-AGNOSTIC - the
// baker only ever sees VolumeFrame, so nothing about the simulation that produced it appears here. The
// runtime streams frames out of this and feeds the raw NanoVDB bytes to the PNanoVDB raymarch.
//
// Layout (little-endian; the engine is x64):
//   Header : magic[8]="HBVOL\0\0\0", u32 version, u32 flags, u32 frameCount, f32 fps, u32 codec,
//            u32 fieldCount, f32 worldMin[3], f32 worldMax[3], i32 dim[3], u64 sourceHash
//   Fields : fieldCount x { str name, u32 type(1=Scalar,3=Vector3), u32 gridType(0=Float,1=Vec3f) }
//   Index  : frameCount x { f32 timeSec, fieldCount x { u64 offset, u64 size, u32 active, f32 min, f32 max } }
//            (offset is relative to the start of the Payload section)
//   Payload: raw NanoVDB grid blobs
#pragma once

#include "Core/Types.h"

#include <cstring>
#include <string>
#include <vector>

namespace hbe::volume {

inline constexpr char        kHbvolMagic[8] = {'H', 'B', 'V', 'O', 'L', '\0', '\0', '\0'};
inline constexpr u32         kHbvolVersion = 1;
enum class HbvolCodec : u32 { None = 0 };           // payload compression (None for now)
enum class HbvolGridType : u32 { Float = 0, Vec3f = 1 };

// Minimal append-only little-endian writer.
struct ByteWriter {
    std::vector<u8> buf;
    void raw(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void u32v(u32 v) { raw(&v, 4); }
    void i32v(i32 v) { raw(&v, 4); }
    void u64v(u64 v) { raw(&v, 8); }
    void f32v(f32 v) { raw(&v, 4); }
    void str(const std::string& s) {
        u32v(static_cast<u32>(s.size()));
        raw(s.data(), s.size());
    }
    usize size() const { return buf.size(); }
};

// Minimal bounds-checked little-endian reader over a borrowed buffer.
struct ByteReader {
    const u8* p = nullptr;
    const u8* end = nullptr;
    bool      ok = true;
    ByteReader(const u8* data, usize n) : p(data), end(data + n) {}

    template <class T> T read() {
        T v{};
        if (!ok || p + sizeof(T) > end) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    u32 u32v() { return read<u32>(); }
    i32 i32v() { return read<i32>(); }
    u64 u64v() { return read<u64>(); }
    f32 f32v() { return read<f32>(); }
    std::string str() {
        const u32 n = u32v();
        if (!ok || p + n > end) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    // Borrow `n` bytes (nullptr on overrun); does not copy.
    const u8* take(usize n) {
        if (!ok || p + n > end) { ok = false; return nullptr; }
        const u8* r = p;
        p += n;
        return r;
    }
    usize consumed(const u8* base) const { return static_cast<usize>(p - base); }
};

} // namespace hbe::volume
