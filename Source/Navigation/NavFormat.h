// Navigation/NavFormat.h - the `.hbnav` on-disk navigation asset.
//
// A .hbnav is a persistent, TILED navmesh baked by the editor (Recast) and streamed
// by the runtime (Detour). It is deliberately independent of the level/scene: the
// world geometry streams in and out around the player, but the navigation data lives
// here on disk and is streamed by its OWN manager, so AI can query navigation beyond
// the currently loaded visual geometry.
//
// LAYOUT (little-endian; all offsets are payload-relative unless noted):
//   Header        - magic/version, bake settings (cell/tile/origin), grid extent,
//                   a source hash for stale-bake detection, and the absolute file
//                   offset + size of the payload.
//   AgentProfile* - one per baked agent size (Human / Large / ...). Each owns a
//                   contiguous run of tile records ([firstTile, firstTile+tileCount)).
//                   The format supports many; the current baker emits one.
//   TileRecord*   - one per (x,y) tile COLUMN: its coords, world AABB, how many
//                   Detour layers it holds, and where the column's compressed blob
//                   lives in the payload. This directory is small - it is what lets
//                   the streamer locate a tile WITHOUT loading the whole navmesh.
//   Payload       - per column, a run of [u32 layerSize][compressed Detour layer]
//                   pairs (the zlib-compressed dtTileCache layers; see NavTileCodec).
//
// This header names NO Detour types - the payload is opaque bytes here and is only
// interpreted by NavMesh.cpp. Gameplay code never touches this file.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {
namespace nav {

inline constexpr char kNavMagic[8] = {'H', 'B', 'N', 'A', 'V', '\0', '\0', '\0'};
inline constexpr u32 kNavVersion = 1;
// Every allocation is sanity-clamped against these before it is trusted - a .hbnav
// can arrive from a pack authored by anyone, so the loader treats it as hostile.
inline constexpr u32 kNavMaxProfiles = 64;
inline constexpr u32 kNavMaxTiles = 1u << 20;      // 1M columns (a 1000x1000 tile grid)
inline constexpr u32 kNavMaxLayersPerTile = 128;   // DT_TILECACHE hard-limits far below this

enum class NavStatus : u32 {
    None = 0,   // never loaded
    Loaded,     // parsed OK
    Missing,    // file not found / unreadable
    Corrupt,    // bad magic / truncated / failed sanity checks
};

// One baked agent size. Tiles are baked per profile (agent radius is eroded into the
// voxel data), so different agents need different tiles - hence the [firstTile,
// firstTile+tileCount) run into the shared tile directory.
struct NavAgentProfile {
    std::string name = "Human";
    f32 radius = 0.6f;
    f32 height = 2.0f;
    f32 maxClimb = 0.9f;
    f32 maxSlopeDeg = 45.0f;
    u32 firstTile = 0;
    u32 tileCount = 0;
};

// One (x,y) tile column. `x`/`y` may be negative (worlds extend in every direction).
// The column's compressed Detour layers live at [payloadOffset, payloadOffset+size)
// within the payload; each is prefixed by a u32 length so the reader can split them.
struct NavTileRecord {
    i32 x = 0;
    i32 y = 0;
    f32 bmin[3] = {0, 0, 0};
    f32 bmax[3] = {0, 0, 0};
    u32 layerCount = 0;
    u64 payloadOffset = 0; // relative to payload start
    u32 payloadSize = 0;   // total bytes for all of this column's layers
};

// The parsed directory of a .hbnav. The PAYLOAD is not held here unless the file was
// served from a pack (residentPayload) - for a loose file, tile blobs are read on
// demand via ReadTileBlob so the whole navmesh never has to be resident.
struct NavMeshData {
    NavStatus status = NavStatus::None;
    u32 version = 0;
    u64 sourceHash = 0;

    f32 cellSize = 0.3f;
    f32 cellHeight = 0.2f;
    i32 tileVoxels = 48;      // tile edge length in cells
    f32 tileWorldSize = 0.0f; // = tileVoxels * cellSize
    f32 origin[3] = {0, 0, 0};
    i32 gridMinX = 0, gridMinY = 0, gridMaxX = 0, gridMaxY = 0;

    std::vector<NavAgentProfile> profiles;
    std::vector<NavTileRecord> tiles;

    // Payload access. Exactly one of these is used:
    //  * residentPayload non-empty  -> whole payload in RAM (came through the VFS/pack).
    //  * else filePath + payloadFileOffset -> per-tile seeks against the loose file.
    std::filesystem::path filePath;
    u64 payloadFileOffset = 0; // absolute offset of the payload within the file
    u64 payloadSize = 0;
    std::vector<u8> residentPayload;

    bool Valid() const { return status == NavStatus::Loaded && !tiles.empty(); }
};

// Parse a .hbnav's header + directory. Reads the payload into memory ONLY when the
// file cannot be opened as a loose file (i.e. it lives inside a mounted .uap pack);
// otherwise it records the offset for on-demand ReadTileBlob seeks. Never throws;
// returns a NavMeshData whose `status` says what happened.
NavMeshData LoadNavMesh(const std::filesystem::path& path);

// Fetch one column's raw payload bytes (the [u32 len][layer]... run). Returns false
// if the record is out of range or the read fails. For a resident payload this slices
// RAM; for a loose file it seeks and reads exactly payloadSize bytes.
bool ReadTileBlob(const NavMeshData& data, const NavTileRecord& rec, std::vector<u8>& out);

// --- Baking (used by the editor NavBaker; harmless in the runtime) ----------

// One column being written: its coords/bounds and its already-compressed Detour
// layers (each element is one dtBuildTileCacheLayer output blob).
struct NavTileBuild {
    i32 x = 0;
    i32 y = 0;
    f32 bmin[3] = {0, 0, 0};
    f32 bmax[3] = {0, 0, 0};
    std::vector<std::vector<u8>> layers;
};

// Everything the header records, assembled by the baker.
struct NavBuildHeader {
    u64 sourceHash = 0;
    f32 cellSize = 0.3f;
    f32 cellHeight = 0.2f;
    i32 tileVoxels = 48;
    f32 tileWorldSize = 0.0f;
    f32 origin[3] = {0, 0, 0};
    i32 gridMinX = 0, gridMinY = 0, gridMaxX = 0, gridMaxY = 0;
    std::vector<NavAgentProfile> profiles; // profile.firstTile/tileCount filled by WriteNavMesh
};

// Serialise header + profiles + directory + payload into a byte buffer. `columns`
// must be ordered so each profile's tiles are contiguous; the profiles' firstTile/
// tileCount are (re)derived from `profileTileCounts` (per-profile column counts, in
// profile order). Returns the bytes to write to disk; empty on inconsistency.
std::vector<u8> WriteNavMesh(const NavBuildHeader& hdr,
                             const std::vector<NavTileBuild>& columns,
                             const std::vector<u32>& profileTileCounts);

} // namespace nav
} // namespace hbe
