// Assets/UAP.cpp
#include "Assets/UAP.h"

#include "Core/JobSystem.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <compressapi.h> // Windows Compression API (Cabinet.lib)

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace hbe::uap {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Pack compression algorithm: LZMS maximizes ratio (decompression is still
// fast); the id is stored in the pack header so readers stay in sync.
constexpr u32 kCompressionAlgorithm = COMPRESS_ALGORITHM_LZMS;

std::optional<std::vector<u8>> CompressBytes(const std::vector<u8>& input, u32 algorithm) {
    COMPRESSOR_HANDLE comp = nullptr;
    if (!::CreateCompressor(algorithm, nullptr, &comp)) return std::nullopt;
    SIZE_T needed = 0;
    ::Compress(comp, input.data(), input.size(), nullptr, 0, &needed);
    std::vector<u8> out(needed);
    SIZE_T written = 0;
    const bool ok =
        ::Compress(comp, input.data(), input.size(), out.data(), out.size(), &written);
    ::CloseCompressor(comp);
    if (!ok) return std::nullopt;
    out.resize(written);
    return out;
}

std::optional<std::vector<u8>> DecompressBytes(const std::vector<u8>& input, u64 rawSize,
                                               u32 algorithm) {
    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (!::CreateDecompressor(algorithm, nullptr, &decomp)) return std::nullopt;
    std::vector<u8> out(static_cast<usize>(rawSize));
    SIZE_T written = 0;
    const bool ok = ::Decompress(decomp, input.data(), input.size(), out.data(),
                                 out.size(), &written);
    ::CloseDecompressor(decomp);
    if (!ok || written != rawSize) return std::nullopt;
    return out;
}

// Reversible obfuscation of a stored entry path (v4+): XOR each byte with a
// position-dependent key so a hex-dump of the pack shows scrambled bytes instead
// of readable asset filenames. Symmetric - the same call encodes and decodes. This
// is a casual-datamining deterrent, NOT encryption.
void ScramblePath(std::string& s) {
    static constexpr u8 kKey[8] = {0x5A, 0xC3, 0x17, 0xE9, 0x42, 0xBD, 0x6F, 0x91};
    for (usize i = 0; i < s.size(); ++i) {
        const u8 k = static_cast<u8>(kKey[i & 7] ^ static_cast<u8>(i * 31u + 7u));
        s[i] = static_cast<char>(static_cast<u8>(s[i]) ^ k);
    }
}

bool IsPackableExtension(const fs::path& ext) {
    // Every asset type the runtime loads through the VFS must be packable, or it
    // goes missing in shipped builds: meshes/textures/fonts (.uaf), scenes
    // (.hbscene), materials (.hbmat), Art Editor paint canvases (.hbpaint),
    // audio events (.hbevent), visual-script graphs (.hbschem), branching dialogue
    // (.hbdialogue), cutscenes (.hbcutscene) and adaptive-music graphs (.hbmusic).
    return ext == ".uaf" || ext == ".hbscene" || ext == ".hbmat" ||
           ext == ".hbpaint" || ext == ".hbevent" || ext == ".hbschem" ||
           ext == ".hbdialogue" || ext == ".hbcutscene" || ext == ".hbmusic";
}

// Loads `path -> slot` assignments from the manifest (empty map if missing).
std::unordered_map<std::string, u32> LoadManifest(const fs::path& manifestPath) {
    std::unordered_map<std::string, u32> slots;
    std::ifstream in(manifestPath);
    if (!in) return slots;
    try {
        json j;
        in >> j;
        // NOTE: don't call .items() on a j.value(...) temporary - the proxy
        // would outlive it (C++20 range-for doesn't extend that lifetime).
        if (const auto it = j.find("slots"); it != j.end() && it->is_object()) {
            for (const auto& [path, slot] : it->items()) {
                slots[path] = slot.get<u32>();
            }
        }
    } catch (const std::exception&) {
        HBE_WARN("UAP: manifest '{}' is corrupt; slots will be reassigned.",
                 manifestPath.string());
        slots.clear();
    }
    return slots;
}

bool SaveManifest(const fs::path& manifestPath,
                  const std::unordered_map<std::string, u32>& slots) {
    // Sorted by slot so diffs of the manifest stay readable.
    std::map<u32, std::string> bySlot;
    for (const auto& [path, slot] : slots) bySlot[slot] = path;
    json j;
    j["version"] = 1;
    j["slotsPerPack"] = kSlotsPerPack;
    auto& out = j["slots"] = json::object();
    for (const auto& [slot, path] : bySlot) out[path] = slot;

    std::error_code ec;
    fs::create_directories(manifestPath.parent_path(), ec);
    std::ofstream file(manifestPath);
    if (!file) return false;
    file << j.dump(2);
    return true;
}

} // namespace

std::optional<PackBuildResult> WritePacks(const fs::path& outDir, const std::string& baseName,
                                          const fs::path& rootDir,
                                          const fs::path& manifestPath,
                                          const WriteOptions& options) {
    std::error_code ec;
    if (!fs::exists(rootDir, ec)) return std::nullopt;

    // Current cooked files (relative path -> size). Extra files (shaders, the
    // project file) may live outside rootDir, so their real source is tracked
    // separately and read from there when packing.
    std::map<std::string, u64> files; // sorted: new-slot assignment is stable
    std::unordered_map<std::string, fs::path> sourceOf; // virtualPath -> on-disk source
    for (const auto& it : fs::recursive_directory_iterator(rootDir, ec)) {
        if (!it.is_regular_file()) continue;
        if (!IsPackableExtension(it.path().extension())) continue;
        const std::string rel = fs::relative(it.path(), rootDir, ec).generic_string();
        if (options.filter && !options.filter->count(rel)) continue;
        files[rel] = static_cast<u64>(fs::file_size(it.path(), ec));
    }
    if (options.extras) {
        for (const ExtraFile& e : *options.extras) {
            if (e.virtualPath.empty() || !fs::exists(e.source, ec)) continue;
            files[e.virtualPath] = static_cast<u64>(fs::file_size(e.source, ec));
            sourceOf[e.virtualPath] = e.source;
        }
    }
    if (files.empty()) {
        HBE_WARN("UAP: nothing to pack under '{}'.", rootDir.string());
        return std::nullopt;
    }

    // Slot assignment: existing assets KEEP their slot; deleted assets free
    // theirs (no shifting); new assets fill the lowest free slot. `compact`
    // discards the prior assignment so everything packs into dense slots 0..N-1
    // (the fewest packs) - used for full exports where pack stability is moot.
    std::unordered_map<std::string, u32> slots =
        options.compact ? std::unordered_map<std::string, u32>{} : LoadManifest(manifestPath);
    for (auto it = slots.begin(); it != slots.end();) {
        it = files.count(it->first) ? std::next(it) : slots.erase(it);
    }
    std::set<u32> used;
    for (const auto& [path, slot] : slots) used.insert(slot);
    for (const auto& [path, size] : files) {
        if (slots.count(path)) continue;
        u32 slot = 0;
        while (used.count(slot)) ++slot; // lowest free slot
        slots[path] = slot;
        used.insert(slot);
    }
    if (!SaveManifest(manifestPath, slots)) {
        HBE_ERROR("UAP: cannot write manifest '{}'.", manifestPath.string());
        return std::nullopt;
    }

    const u32 maxSlot = used.empty() ? 0 : *used.rbegin();
    const u32 packCount = maxSlot / kSlotsPerPack + 1;

    // Slot -> path lookup for the writer.
    std::unordered_map<u32, std::string> bySlot;
    for (const auto& [path, slot] : slots) bySlot[slot] = path;

    fs::create_directories(outDir, ec);
    const u32 algorithm = options.compress ? kCompressionAlgorithm : 0;
    PackBuildResult result;
    result.assetCount = static_cast<u32>(files.size());
    result.packCount = packCount;

    // --- Load + compress every assigned slot's bytes IN PARALLEL -------------
    // The per-file read + LZMS compress is the bulk of pack time and is fully
    // independent per file, so fan it out across the job system (CompressBytes
    // makes its own compressor each call - reentrant). The pack files are still
    // written sequentially below from these preloaded bytes: identical bytes,
    // TOC and verify, just no longer serialized on the calling thread. When the
    // job system isn't running (e.g. the --pack CLI before engine startup),
    // ParallelFor falls back to a serial loop.
    struct LoadItem {
        u32 slot = 0;
        std::string path;
        fs::path src;
    };
    std::vector<LoadItem> items;
    items.reserve(bySlot.size());
    for (const auto& [slot, path] : bySlot) {
        const auto srcIt = sourceOf.find(path);
        items.push_back({slot, path,
                         srcIt != sourceOf.end() ? srcIt->second : (rootDir / fs::path(path))});
    }
    struct Blob {
        u64 rawSize = 0;
        std::vector<u8> stored; // file bytes, possibly compressed
        bool ok = true;
    };
    std::vector<Blob> loaded(items.size());
    std::atomic<bool> loadFailed{false};
    jobs::ParallelFor(static_cast<u32>(items.size()), 1, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const LoadItem& li = items[i];
            Blob& b = loaded[i];
            std::ifstream in(li.src, std::ios::binary | std::ios::ate);
            if (!in) {
                HBE_ERROR("UAP: failed to read '{}' while packing.", li.path);
                b.ok = false;
                loadFailed.store(true, std::memory_order_relaxed);
                continue;
            }
            const std::streamsize size = in.tellg();
            in.seekg(0);
            b.stored.resize(static_cast<usize>(size));
            in.read(reinterpret_cast<char*>(b.stored.data()), size);
            b.rawSize = static_cast<u64>(size);
            if (algorithm != 0) {
                // Keep the original when compression doesn't shrink it.
                if (auto packed = CompressBytes(b.stored, algorithm);
                    packed && packed->size() < b.stored.size()) {
                    b.stored = std::move(*packed);
                }
            }
        }
    });
    if (loadFailed.load(std::memory_order_relaxed)) return std::nullopt;

    std::unordered_map<u32, usize> blobIndexBySlot;
    blobIndexBySlot.reserve(items.size());
    for (usize i = 0; i < items.size(); ++i) {
        blobIndexBySlot[items[i].slot] = i;
        result.rawBytes += loaded[i].rawSize;
        result.packedBytes += loaded[i].stored.size();
    }

    for (u32 p = 0; p < packCount; ++p) {
        const fs::path packFile = outDir / (baseName + "_" + std::to_string(p) + ".uap");

        // Load (and optionally compress) this chunk's blobs, then lay out the
        // TOC from the stored sizes.
        struct SlotEntry {
            std::string path;
            u64 offset = 0;
            u64 rawSize = 0;
            std::vector<u8> stored; // file bytes, possibly compressed
        };
        std::vector<SlotEntry> chunk(kSlotsPerPack);
        u64 tocSize = 4 + 3 * sizeof(u32);
        for (u32 s = 0; s < kSlotsPerPack; ++s) {
            SlotEntry& e = chunk[s];
            if (auto it = bySlot.find(p * kSlotsPerPack + s); it != bySlot.end()) {
                e.path = it->second;
                // Bytes were read + compressed in the parallel pass above.
                Blob& b = loaded[blobIndexBySlot.at(p * kSlotsPerPack + s)];
                e.rawSize = b.rawSize;
                e.stored = std::move(b.stored);
            }
            tocSize += sizeof(u32) + e.path.size() + 3 * sizeof(u64);
        }
        u64 offset = tocSize;
        for (SlotEntry& e : chunk) {
            if (e.path.empty()) continue;
            e.offset = offset;
            offset += e.stored.size();
        }

        std::ofstream out(packFile, std::ios::binary | std::ios::trunc);
        if (!out) {
            HBE_ERROR("UAP: cannot write '{}'.", packFile.string());
            return std::nullopt;
        }
        const auto pod = [&out](const auto& v) {
            out.write(reinterpret_cast<const char*>(&v), sizeof(v));
        };
        out.write(kMagic, 4);
        pod(kVersion);
        pod(kSlotsPerPack);
        pod(algorithm);
        for (const SlotEntry& e : chunk) {
            pod(static_cast<u32>(e.path.size()));
            if (!e.path.empty()) {
                std::string scrambled = e.path; // obfuscate on disk (v4); reader decodes
                ScramblePath(scrambled);
                out.write(scrambled.data(), static_cast<std::streamsize>(scrambled.size()));
            }
            pod(e.offset);
            pod(static_cast<u64>(e.stored.size()));
            pod(e.rawSize);
        }
        for (const SlotEntry& e : chunk) {
            if (e.path.empty()) continue;
            out.write(reinterpret_cast<const char*>(e.stored.data()),
                      static_cast<std::streamsize>(e.stored.size()));
        }
    }

    HBE_INFO("UAP: packed {} assets into {} pack(s) of {} slots ('{}_N.uap'){}.",
             result.assetCount, result.packCount, kSlotsPerPack, baseName,
             options.compress
                 ? " at " + std::to_string(result.rawBytes ? result.packedBytes * 100 /
                                                                 result.rawBytes
                                                           : 100) + "% of source size"
                 : "");
    return result;
}

bool PackReader::Open(const fs::path& packFile) {
    entries_.clear();
    index_.clear();
    file_ = packFile;

    std::ifstream in(packFile, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    in.read(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return false;
    const auto pod = [&in](auto& v) {
        in.read(reinterpret_cast<char*>(&v), sizeof(v));
    };
    u32 version = 0, count = 0;
    pod(version);
    pod(count);
    if (!in || version > kVersion || count > 100000) return false;
    algorithm_ = 0;
    if (version >= 3) pod(algorithm_);

    for (u32 i = 0; i < count; ++i) {
        u32 len = 0;
        pod(len);
        if (!in || len > 4096) return false;
        Entry e;
        e.slot = i;
        e.path.resize(len);
        if (len) in.read(e.path.data(), len);
        if (len && version >= 4) ScramblePath(e.path); // de-obfuscate v4 paths
        pod(e.offset);
        pod(e.size);
        if (version >= 3) {
            pod(e.rawSize);
        } else {
            e.rawSize = e.size; // v2: always stored verbatim
        }
        if (!in) return false;
        if (e.path.empty()) continue; // free slot
        index_[e.path] = entries_.size();
        entries_.push_back(std::move(e));
    }
    return true;
}

std::optional<std::vector<u8>> PackReader::Read(const std::string& relPath) const {
    const auto it = index_.find(relPath);
    if (it == index_.end()) return std::nullopt;
    const Entry& e = entries_[it->second];

    std::ifstream in(file_, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(static_cast<std::streamoff>(e.offset));
    std::vector<u8> bytes(e.size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(e.size));
    if (!in) return std::nullopt;
    if (e.size == e.rawSize) return bytes; // stored verbatim
    auto raw = DecompressBytes(bytes, e.rawSize, algorithm_);
    if (!raw) {
        HBE_ERROR("UAP: failed to decompress '{}' from '{}'.", relPath, file_.string());
    }
    return raw;
}

bool PackSet::Open(const fs::path& dir, const std::string& baseName) {
    readers_.clear();
    for (u32 p = 0;; ++p) {
        const fs::path packFile = dir / (baseName + "_" + std::to_string(p) + ".uap");
        std::error_code ec;
        if (!fs::exists(packFile, ec)) break;
        PackReader reader;
        if (!reader.Open(packFile)) return false;
        readers_.push_back(std::move(reader));
    }
    return !readers_.empty();
}

u32 PackSet::AssetCount() const {
    u32 n = 0;
    for (const PackReader& r : readers_) n += static_cast<u32>(r.Entries().size());
    return n;
}

std::vector<Entry> PackSet::Entries() const {
    std::vector<Entry> all;
    for (usize p = 0; p < readers_.size(); ++p) {
        for (Entry e : readers_[p].Entries()) {
            e.slot += static_cast<u32>(p) * kSlotsPerPack; // globalize
            all.push_back(std::move(e));
        }
    }
    return all;
}

bool PackSet::Contains(const std::string& relPath) const {
    for (const PackReader& r : readers_) {
        if (r.Contains(relPath)) return true;
    }
    return false;
}

std::optional<std::vector<u8>> PackSet::Read(const std::string& relPath) const {
    for (const PackReader& r : readers_) {
        if (r.Contains(relPath)) return r.Read(relPath);
    }
    return std::nullopt;
}

} // namespace hbe::uap
