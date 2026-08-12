// Navigation/NavFormat.cpp - see NavFormat.h.
#include "Navigation/NavFormat.h"

#include "Assets/VFS.h"
#include "Core/Log.h"

#include <cstring>
#include <fstream>

namespace hbe {
namespace nav {
namespace {

// Little-endian append writer (POD only; the engine ships on LE targets).
struct ByteWriter {
    std::vector<u8> buf;
    void raw(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    template <class T>
    void pod(const T& v) { raw(&v, sizeof(T)); }
    void str(const std::string& s) {
        pod<u32>(static_cast<u32>(s.size()));
        raw(s.data(), s.size());
    }
    usize size() const { return buf.size(); }
};

// Bounds-checked cursor reader. Any overrun latches `ok=false`; every read after that
// is a no-op, so a truncated file fails the first validity check instead of reading
// past the buffer.
struct ByteReader {
    const u8* p = nullptr;
    usize n = 0;
    usize cur = 0;
    bool ok = true;
    ByteReader(const u8* data, usize len) : p(data), n(len) {}
    void raw(void* out, usize count) {
        if (!ok || cur + count > n) { ok = false; return; }
        std::memcpy(out, p + cur, count);
        cur += count;
    }
    template <class T>
    T pod() {
        T v{};
        raw(&v, sizeof(T));
        return v;
    }
    std::string str(u32 maxLen) {
        const u32 len = pod<u32>();
        if (!ok || len > maxLen || cur + len > n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + cur), len);
        cur += len;
        return s;
    }
};

// Parse header + directory out of a full-file byte buffer. Fills everything except
// the payload plumbing (the caller decides RAM vs seek). Returns Loaded/Corrupt.
NavStatus ParseDirectory(const u8* bytes, usize len, NavMeshData& out) {
    ByteReader r(bytes, len);
    char magic[8] = {0};
    r.raw(magic, 8);
    if (!r.ok || std::memcmp(magic, kNavMagic, 8) != 0) return NavStatus::Corrupt;
    out.version = r.pod<u32>();
    if (!r.ok || out.version != kNavVersion) return NavStatus::Corrupt;
    (void)r.pod<u32>(); // flags (reserved)
    out.sourceHash = r.pod<u64>();
    out.cellSize = r.pod<f32>();
    out.cellHeight = r.pod<f32>();
    out.tileVoxels = r.pod<i32>();
    out.tileWorldSize = r.pod<f32>();
    r.raw(out.origin, sizeof(out.origin));
    out.gridMinX = r.pod<i32>();
    out.gridMinY = r.pod<i32>();
    out.gridMaxX = r.pod<i32>();
    out.gridMaxY = r.pod<i32>();
    const u32 profileCount = r.pod<u32>();
    const u32 tileCount = r.pod<u32>();
    const u64 payloadOffset = r.pod<u64>();
    const u64 payloadSize = r.pod<u64>();
    if (!r.ok) return NavStatus::Corrupt;
    // Sanity clamps BEFORE any allocation (untrusted input / allocation-bomb guard).
    if (out.cellSize <= 0.0f || out.tileVoxels <= 0 || out.tileWorldSize <= 0.0f)
        return NavStatus::Corrupt;
    if (profileCount == 0 || profileCount > kNavMaxProfiles) return NavStatus::Corrupt;
    if (tileCount == 0 || tileCount > kNavMaxTiles) return NavStatus::Corrupt;

    out.profiles.resize(profileCount);
    for (u32 i = 0; i < profileCount; ++i) {
        NavAgentProfile& pr = out.profiles[i];
        pr.name = r.str(256);
        pr.radius = r.pod<f32>();
        pr.height = r.pod<f32>();
        pr.maxClimb = r.pod<f32>();
        pr.maxSlopeDeg = r.pod<f32>();
        pr.firstTile = r.pod<u32>();
        pr.tileCount = r.pod<u32>();
        if (!r.ok) return NavStatus::Corrupt;
        // A profile's tile run must lie inside the directory.
        if (pr.tileCount > tileCount || pr.firstTile > tileCount - pr.tileCount)
            return NavStatus::Corrupt;
    }

    out.tiles.resize(tileCount);
    for (u32 i = 0; i < tileCount; ++i) {
        NavTileRecord& t = out.tiles[i];
        t.x = r.pod<i32>();
        t.y = r.pod<i32>();
        r.raw(t.bmin, sizeof(t.bmin));
        r.raw(t.bmax, sizeof(t.bmax));
        t.layerCount = r.pod<u32>();
        t.payloadOffset = r.pod<u64>();
        t.payloadSize = r.pod<u32>();
        if (!r.ok) return NavStatus::Corrupt;
        if (t.layerCount > kNavMaxLayersPerTile) return NavStatus::Corrupt;
        // Every column's blob must sit inside the payload (guard the u64 add against
        // overflow the same way VolumeAsset does).
        if (t.payloadSize > 0 &&
            (t.payloadOffset > payloadSize || t.payloadSize > payloadSize - t.payloadOffset))
            return NavStatus::Corrupt;
    }

    out.payloadFileOffset = payloadOffset;
    out.payloadSize = payloadSize;
    // The header's payload offset must be where the directory actually ended, and the
    // declared payload must fit the file.
    if (payloadOffset != r.cur) return NavStatus::Corrupt;
    if (payloadOffset > len || payloadSize > len - payloadOffset) return NavStatus::Corrupt;
    return NavStatus::Loaded;
}

} // namespace

NavMeshData LoadNavMesh(const std::filesystem::path& path) {
    NavMeshData out;
    out.filePath = path;

    // A loose file on disk lets us seek per tile (true streaming). If it does not exist
    // as a loose file it may still live inside a mounted pack, which the VFS serves as a
    // whole buffer - small for nav data, and the resident dtNavMesh stays windowed
    // regardless.
    std::error_code ec;
    const bool looseExists = std::filesystem::exists(path, ec) && !ec &&
                             std::filesystem::is_regular_file(path, ec);
    if (looseExists) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            out.status = NavStatus::Missing;
            return out;
        }
        // The directory is a small prefix; read the whole file to parse it, then drop
        // the payload bytes and keep the offset so tiles are read on demand.
        std::vector<u8> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        out.status = ParseDirectory(all.data(), all.size(), out);
        if (out.status != NavStatus::Loaded) {
            HBE_WARN("Nav: '{}' failed to parse ({}).", path.string(),
                     out.status == NavStatus::Corrupt ? "corrupt" : "unreadable");
            return out;
        }
        // Loose file: read tiles on demand (payload NOT held in RAM).
        return out;
    }

    // Pack-served (or absent). vfs::ReadFile is pack-aware; the whole file rides in RAM.
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        out.status = NavStatus::Missing;
        return out;
    }
    out.status = ParseDirectory(bytes->data(), bytes->size(), out);
    if (out.status != NavStatus::Loaded) {
        HBE_WARN("Nav: '{}' failed to parse from pack ({}).", path.string(),
                 out.status == NavStatus::Corrupt ? "corrupt" : "unreadable");
        return out;
    }
    // Keep only the payload slice resident; the directory is already parsed into `out`.
    out.residentPayload.assign(bytes->begin() + static_cast<std::ptrdiff_t>(out.payloadFileOffset),
                               bytes->begin() + static_cast<std::ptrdiff_t>(out.payloadFileOffset + out.payloadSize));
    return out;
}

bool ReadTileBlob(const NavMeshData& data, const NavTileRecord& rec, std::vector<u8>& out) {
    out.clear();
    if (rec.payloadSize == 0) return true; // an empty column is valid (no walkable layers)
    if (rec.payloadOffset > data.payloadSize || rec.payloadSize > data.payloadSize - rec.payloadOffset)
        return false;

    if (!data.residentPayload.empty()) {
        if (rec.payloadOffset + rec.payloadSize > data.residentPayload.size()) return false;
        out.assign(data.residentPayload.begin() + static_cast<std::ptrdiff_t>(rec.payloadOffset),
                   data.residentPayload.begin() + static_cast<std::ptrdiff_t>(rec.payloadOffset + rec.payloadSize));
        return true;
    }

    // Loose-file seek: read exactly this column's bytes, nothing else.
    std::ifstream f(data.filePath, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(data.payloadFileOffset + rec.payloadOffset), std::ios::beg);
    if (!f) return false;
    out.resize(rec.payloadSize);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(rec.payloadSize));
    if (static_cast<u64>(f.gcount()) != rec.payloadSize) {
        out.clear();
        return false;
    }
    return true;
}

std::vector<u8> WriteNavMesh(const NavBuildHeader& hdr,
                             const std::vector<NavTileBuild>& columns,
                             const std::vector<u32>& profileTileCounts) {
    // Validate the profile partition adds up to the column count.
    u64 sum = 0;
    for (u32 c : profileTileCounts) sum += c;
    if (sum != columns.size() || profileTileCounts.size() != hdr.profiles.size()) {
        HBE_WARN("Nav: WriteNavMesh profile partition ({}) != column count ({}).", sum, columns.size());
        return {};
    }

    // 1. Lay out the payload; record each column's payload-relative offset + size.
    ByteWriter payload;
    std::vector<u64> colOffset(columns.size());
    std::vector<u32> colSize(columns.size());
    for (usize i = 0; i < columns.size(); ++i) {
        colOffset[i] = payload.size();
        const usize before = payload.size();
        for (const std::vector<u8>& layer : columns[i].layers) {
            payload.pod<u32>(static_cast<u32>(layer.size()));
            payload.raw(layer.data(), layer.size());
        }
        colSize[i] = static_cast<u32>(payload.size() - before);
    }

    // 2. Header + profiles + directory (variable-length), so payload offset is derived
    //    after they are assembled.
    ByteWriter head;
    head.raw(kNavMagic, 8);
    head.pod<u32>(kNavVersion);
    head.pod<u32>(0); // flags
    head.pod<u64>(hdr.sourceHash);
    head.pod<f32>(hdr.cellSize);
    head.pod<f32>(hdr.cellHeight);
    head.pod<i32>(hdr.tileVoxels);
    head.pod<f32>(hdr.tileWorldSize);
    head.raw(hdr.origin, sizeof(hdr.origin));
    head.pod<i32>(hdr.gridMinX);
    head.pod<i32>(hdr.gridMinY);
    head.pod<i32>(hdr.gridMaxX);
    head.pod<i32>(hdr.gridMaxY);
    head.pod<u32>(static_cast<u32>(hdr.profiles.size()));
    head.pod<u32>(static_cast<u32>(columns.size()));
    // payloadOffset + payloadSize are patched in after we know the header's own size.
    const usize payloadOffsetField = head.size();
    head.pod<u64>(0); // payloadOffset placeholder
    head.pod<u64>(static_cast<u64>(payload.size()));

    // Profiles: derive firstTile from the running partition.
    u32 running = 0;
    for (usize i = 0; i < hdr.profiles.size(); ++i) {
        NavAgentProfile pr = hdr.profiles[i];
        pr.firstTile = running;
        pr.tileCount = profileTileCounts[i];
        running += pr.tileCount;
        head.str(pr.name);
        head.pod<f32>(pr.radius);
        head.pod<f32>(pr.height);
        head.pod<f32>(pr.maxClimb);
        head.pod<f32>(pr.maxSlopeDeg);
        head.pod<u32>(pr.firstTile);
        head.pod<u32>(pr.tileCount);
    }

    // Directory.
    for (usize i = 0; i < columns.size(); ++i) {
        const NavTileBuild& c = columns[i];
        head.pod<i32>(c.x);
        head.pod<i32>(c.y);
        head.raw(c.bmin, sizeof(c.bmin));
        head.raw(c.bmax, sizeof(c.bmax));
        head.pod<u32>(static_cast<u32>(c.layers.size()));
        head.pod<u64>(colOffset[i]);
        head.pod<u32>(colSize[i]);
    }

    // Patch the absolute payload offset now that the header length is known.
    const u64 payloadAbs = static_cast<u64>(head.size());
    std::memcpy(head.buf.data() + payloadOffsetField, &payloadAbs, sizeof(u64));

    // 3. header + payload.
    std::vector<u8> out = std::move(head.buf);
    out.insert(out.end(), payload.buf.begin(), payload.buf.end());
    return out;
}

} // namespace nav
} // namespace hbe
