// Assets/UAP.h - Unified Asset Pack (.uap).
//
// Shipping containers for cooked assets (.uaf, .hbscene). Assets are split
// across chunked packs of kSlotsPerPack FIXED slots each:
//   <base>_0.uap holds global slots [0,49], <base>_1.uap holds [50,99], ...
// An asset keeps its slot for life (assignments persist in a JSON manifest
// next to the project). Deleting an asset frees its slot WITHOUT shifting the
// others - untouched packs stay byte-identical across updates - and the next
// imported asset fills the lowest free slot.
//
// Per-pack layout (version 3):
//   "UAP1" | u32 version | u32 slotCount | u32 algorithm (0 = stored)
//   slotCount * [ u32 pathLen | path bytes (UTF-8, '/') | u64 offset
//                 | u64 storedSize | u64 rawSize ]
//   blobs (offsets are absolute file positions; empty slots have pathLen 0).
// An entry with storedSize == rawSize is stored verbatim; otherwise the blob
// is compressed with the pack's algorithm (Windows Compression API). Version 2
// packs (no algorithm field, single size) still load.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::uap {

inline constexpr char kMagic[4] = {'U', 'A', 'P', '1'};
// v4: entry path strings are obfuscated on disk (reversible scramble) so a shipped
// pack doesn't reveal asset filenames to a casual hex-dump. v2/v3 (plaintext) still
// load. NOT encryption - just a datamining deterrent.
inline constexpr u32 kVersion = 4;
inline constexpr u32 kSlotsPerPack = 50;

struct Entry {
    std::string path; // relative to the packed root, forward slashes
    u64 offset = 0;
    u64 size = 0;     // stored (possibly compressed) byte count
    u64 rawSize = 0;  // decompressed byte count (== size when stored verbatim)
    u32 slot = 0;     // global slot index
};

struct PackBuildResult {
    u32 assetCount = 0;
    u32 packCount = 0;
    u64 rawBytes = 0;    // total input size
    u64 packedBytes = 0; // total stored size (after compression)
};

// An extra file packed under a chosen virtual path, sourced from anywhere on
// disk (not just `rootDir`). Used by shipping to fold engine shaders and the
// project file into the content packs so the build ships only exe + packs + DLLs.
struct ExtraFile {
    std::string virtualPath;        // pack-relative path (forward slashes)
    std::filesystem::path source;   // file to read the bytes from
};

struct WriteOptions {
    // LZMS-compress each asset (skipped per-asset when it doesn't shrink).
    bool compress = false;
    // When set, only these Assets-relative paths are packed (extras are exempt).
    const std::set<std::string>* filter = nullptr;
    // Extra files packed in addition to (and exempt from the filter of) the
    // rootDir content - e.g. compiled shaders, the project file.
    const std::vector<ExtraFile>* extras = nullptr;
    // Ignore the existing manifest's slot assignments and pack into dense slots
    // 0..N-1 (the fewest packs). Use for a fresh full export where stable,
    // byte-identical packs across rebuilds don't matter; leave off for live dev
    // packing so unchanged packs stay cached.
    bool compact = false;
};

// Packs every .uaf / .hbscene / .hbmat under `rootDir` into `<baseName>_<n>.uap`
// files in `outDir`, using (and updating) the slot manifest at `manifestPath`.
std::optional<PackBuildResult> WritePacks(const std::filesystem::path& outDir,
                                          const std::string& baseName,
                                          const std::filesystem::path& rootDir,
                                          const std::filesystem::path& manifestPath,
                                          const WriteOptions& options = {});

// Reads one .uap chunk.
class PackReader {
public:
    bool Open(const std::filesystem::path& packFile);
    // Occupied entries only (empty slots are skipped).
    const std::vector<Entry>& Entries() const { return entries_; }
    bool Contains(const std::string& relPath) const { return index_.count(relPath) != 0; }
    std::optional<std::vector<u8>> Read(const std::string& relPath) const;

private:
    std::filesystem::path file_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, usize> index_;
    u32 algorithm_ = 0; // Windows COMPRESS_ALGORITHM_* id (0 = stored)
};

// Aggregates every `<baseName>_<n>.uap` chunk in a directory.
class PackSet {
public:
    bool Open(const std::filesystem::path& dir, const std::string& baseName);
    u32 PackCount() const { return static_cast<u32>(readers_.size()); }
    u32 AssetCount() const;
    // All occupied entries across chunks.
    std::vector<Entry> Entries() const;
    bool Contains(const std::string& relPath) const;
    std::optional<std::vector<u8>> Read(const std::string& relPath) const;

private:
    std::vector<PackReader> readers_;
};

} // namespace hbe::uap
