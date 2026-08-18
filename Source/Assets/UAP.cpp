// Assets/UAP.cpp
#include "Assets/UAP.h"

#include "Assets/AssetFormats.h" // the single source of truth for packable types
#include "Assets/Compression.h"  // the portable (zstd/zlib/legacy-LZMS) codec seam
#include "Assets/SlotIds.h"      // the asset's OWN pack slot (authority tier 1)

#include "Core/JobSystem.h"
#include "Core/Log.h"

#include <atomic>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace hbe::uap {

namespace fs = std::filesystem;

namespace {

// The portable codec new packs are cooked with. zstd decodes far faster than the old
// LZMS at a comparable ratio and, unlike LZMS, has no OS dependency - so a v5 pack loads
// on any platform. (See Assets/Compression.h for the codec seam and why.)
constexpr comp::Codec kCookCodec = comp::Codec::Zstd;

// Round `n` up to the next multiple of `a` (a must be a power of two).
constexpr u64 AlignUp(u64 n, u64 a) { return (n + (a - 1)) & ~(a - 1); }

// Maps a pack's stored codec field to a portable comp::Codec, honouring the version:
//   v5+ : the field already IS a comp::Codec id.
//   v3/4: the field is a Windows COMPRESS_ALGORITHM_* value - 0 means stored, anything
//         else was LZMS (the only algorithm ever written), read-only on Windows.
//   v2  : no field; always stored verbatim.
comp::Codec ResolveCodec(u32 version, u32 field) {
    if (version >= 5) return static_cast<comp::Codec>(field);
    if (version >= 3) return field == 0 ? comp::Codec::None : comp::Codec::LzmsWin;
    return comp::Codec::None;
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
    // Derived from the ONE registry (Assets/AssetFormats.cpp): every engine asset
    // flagged runtimeLoaded is packable. This used to be a hand-written list that
    // silently fell behind - .hbchar (modular characters), .hbprefab (everything a
    // Spawner spawns), .hbuianim and .hbgi were all runtime-loaded but unpacked,
    // so those features died in shipped builds with no error at all.
    return assets::IsPackable(assets::NormalizeExtension(ext));
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
    // EVERY packable file on disk, filter or no filter. Slots are reserved from
    // this set rather than from `files`, so an asset that this cook does not pack
    // (because BuildSettings::onlyReferenced excluded it) still holds its number -
    // toggling that setting can no longer renumber the assets that DO ship.
    std::vector<std::string> assetKeys;
    u64 scanned = 0; // regular files seen under rootDir (packable or not)
    for (const auto& it : fs::recursive_directory_iterator(rootDir, ec)) {
        if (!it.is_regular_file()) continue;
        ++scanned;
        if (!IsPackableExtension(it.path().extension())) continue;
        const std::string rel = fs::relative(it.path(), rootDir, ec).generic_string();
        assetKeys.push_back(rel);
        if (options.filter && !options.filter->count(rel)) continue;
        files[rel] = static_cast<u64>(fs::file_size(it.path(), ec));
    }
    std::sort(assetKeys.begin(), assetKeys.end());
    // Guard: a populated Assets folder that contributes ZERO packable files means
    // the extension test is broken, not that the project is empty. That exact
    // failure once shipped a build containing only shaders + the project file -
    // every scene silently missing, the game booting to an empty world with no
    // error. Loud here beats a mystery at runtime.
    if (scanned > 0 && files.empty()) {
        HBE_ERROR("UAP: scanned {} file(s) under '{}' but NONE were packable. "
                  "The packable-extension test is almost certainly broken - a shipped "
                  "build would boot with no scenes or assets.",
                  scanned, rootDir.string());
        return std::nullopt;
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

    // --- SLOT ASSIGNMENT ----------------------------------------------------
    // The cook DECIDES nothing it can look up. An asset's slot is a property of
    // the asset, assigned when it was created and stored inside it
    // (Assets/SlotIds.h), so a cook is a pure function of the files on disk and
    // two cooks in a row produce byte-identical packs. The order below is the
    // authority order, and the tiers are disjoint - there is exactly one
    // authority per file:
    //   0. the ledger's reservations for everything that is NOT an asset - the
    //      compiled shaders and `__project.hbproj`. These come FIRST, ahead of even
    //      an embedded id, because they are the only tier those files have: an
    //      asset that loses a contested slot can be repaired permanently by
    //      --migrate-slots, while a displaced shader would be displaced again on
    //      every cook forever. It is reported either way (`displace` below).
    //   1. the id embedded in the asset;
    //   2. the id this manifest remembers for that exact pack key - for the binary
    //      bakes, which can hold no field, and for an asset whose saver rebuilt its
    //      JSON and dropped the key;
    //   3. the lowest free id, in sorted-path order.
    // This is what the old code got wrong twice over: the manifest was the ONLY
    // authority (so a rename was a delete + create), and the shipping cook set
    // `compact`, which threw the manifest away and re-derived dense slots
    // 0..N-1 - so adding one early-sorting asset rewrote every pack file.
    std::vector<std::string> extraKeys;
    for (const auto& [path, size] : files) {
        if (sourceOf.count(path)) extraKeys.push_back(path); // `files` is sorted
    }
    std::map<std::string, u32> remembered = slots::LoadRememberedSlots(manifestPath);
    slots::SlotTable table;
    // Every displacement is a pack that changes for every player, so each one is
    // named. This used to be silent for everything except a tier-1 collision, which
    // is precisely why two whole classes of renumbering went unnoticed.
    u32 displaced = 0;
    const auto displace = [&](const std::string& key, u32 wanted) {
        ++displaced;
        std::string holder = "(unknown)";
        for (const auto& [k, s] : table.All())
            if (s == wanted) { holder = k; break; }
        HBE_WARN("UAP: '{}' wanted pack slot {} but '{}' holds it; '{}' is renumbered for "
                 "this cook, so the pack it lands in changes for everyone who already has "
                 "the old one.",
                 key, wanted, holder, key);
    };

    // NON-ASSET RESERVATIONS FIRST, from the ledger, whether or not THIS cook packs
    // them. The compiled shaders and `__project.hbproj` ship as ExtraFiles in the
    // same slot space and can carry no embedded id, so they exist only here - and a
    // cook that has no extras at all (the dev `--pack`) would otherwise treat all
    // 100+ of their numbers as free and hand them to assets, permanently.
    for (const auto& [key, slot] : remembered)
        if (!slots::IsAssetSlotKey(key)) table.Claim(key, slot);

    std::vector<std::string> pending; // no embedded id
    for (const std::string& rel : assetKeys) {
        const std::optional<u32> embedded = slots::ReadSlot(rootDir / fs::path(rel));
        if (!embedded || *embedded == slots::kUnassigned) {
            pending.push_back(rel);
            continue;
        }
        if (table.Claim(rel, *embedded)) continue;
        // Two files claiming one id - two assets copied between projects, most
        // likely, or an asset whose embedded id lands on an ExtraFile reservation.
        // The lexicographically lower path (or the reservation) keeps it and this
        // one is re-numbered, so two machines converge on the same packing - but
        // `displace` says so, loudly and by name, because the loser's number
        // changes and that costs a patch.
        displace(rel, *embedded);
        HBE_WARN("UAP: run --migrate-slots --apply to give '{}' a permanent id and make "
                 "that fix stick.",
                 rel);
        pending.push_back(rel);
    }

    std::vector<std::string> unassigned; // tier 3
    for (const std::string& rel : pending) {
        const auto it = remembered.find(rel);
        if (it == remembered.end()) {
            unassigned.push_back(rel);
        } else if (!table.Claim(rel, it->second)) {
            displace(rel, it->second); // it HAD a number and lost it - never silent
            unassigned.push_back(rel);
        }
    }
    for (const std::string& virt : extraKeys) {
        if (table.SlotOf(virt) != slots::kUnassigned) continue; // reserved above
        const auto it = remembered.find(virt);
        if (it == remembered.end()) {
            unassigned.push_back(virt);
        } else if (!table.Claim(virt, it->second)) {
            displace(virt, it->second);
            unassigned.push_back(virt);
        }
    }
    u32 freshIds = 0;
    for (const std::string& key : unassigned) {
        table.ClaimLowestFree(key);
        ++freshIds;
    }
    // NOT gated on `pending` being non-empty. That gate meant a displaced SHADER -
    // the exact victim of a shared ledger going wrong - produced no output at all.
    if (freshIds != 0) {
        HBE_INFO("UAP: {} file(s) were numbered for this cook (no id in the file, none "
                 "remembered, or the one they wanted was taken). Run --migrate-slots to give "
                 "them permanent ids.",
                 freshIds);
    }
    if (displaced != 0) {
        HBE_WARN("UAP: {} file(s) were DISPLACED from the slot they previously held (listed "
                 "above). Every pack containing one of them changes, so this build is a "
                 "larger patch than it needs to be.",
                 displaced);
    }

    // The ledger is MERGED, not overwritten: an asset that is gone loses its entry
    // (which is exactly what frees its id for the next thing created), a non-asset
    // entry is kept whether or not this cook packed extras, and everything the
    // allocator holds is recorded. Writing table.All() wholesale was safe only while
    // the dev cook and the ship cook had separate manifests; sharing one ledger (the
    // point of Project::SlotManifestPath) means a `--pack` with no extras would
    // otherwise erase every shader reservation on its way past.
    slots::MergeRememberedSlots(rootDir, table, remembered);
    if (!slots::SaveRememberedSlots(manifestPath, remembered)) {
        HBE_ERROR("UAP: cannot write manifest '{}'.", manifestPath.string());
        return std::nullopt;
    }

    // Slot -> path lookup for the writer, over the files this cook actually packs.
    // The pack count follows the highest PACKED slot, not the highest reserved
    // one - a filtered cook must not emit six empty packs because an unreferenced
    // asset happens to sit at slot 300.
    std::unordered_map<u32, std::string> bySlot;
    u32 maxSlot = 0;
    for (const auto& [path, size] : files) {
        const u32 slot = table.SlotOf(path);
        if (slot == slots::kUnassigned) {
            HBE_ERROR("UAP: '{}' ended up with no pack slot - refusing to write packs that "
                      "would silently omit it.",
                      path);
            return std::nullopt;
        }
        bySlot[slot] = path;
        maxSlot = std::max(maxSlot, slot);
    }
    const u32 packCount = maxSlot / kSlotsPerPack + 1;

    fs::create_directories(outDir, ec);
    const comp::Codec codec = options.compress ? kCookCodec : comp::Codec::None;
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
        u64 hash = 0;           // comp::Hash64 over the RAW bytes (dedup + integrity)
        std::vector<u8> stored; // file bytes, possibly compressed
        bool compressed = false;// true when `stored` is codec-compressed (size < raw)
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
            // Hash the RAW bytes: compression-independent, so it is stable across a
            // re-cook and is what within-pack dedup keys on.
            b.hash = comp::Hash64(b.stored.data(), b.stored.size());
            if (codec != comp::Codec::None) {
                // Keep the original when compression doesn't shrink it (this also skips
                // storing a larger blob for already-compressed content like BC textures,
                // meshopt-encoded geometry or compressed audio).
                if (auto packed = comp::Compress(codec, b.stored.data(), b.stored.size());
                    packed && packed->size() < b.stored.size()) {
                    b.stored = std::move(*packed);
                    b.compressed = true;
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
    }

    // v5 header: magic(4) + version + slotCount + codec + alignment = 4 + 4*u32.
    constexpr u64 kHeaderSize = 4 + 4 * sizeof(u32);
    // v5 TOC record: pathLen(u32) + path + offset(u64) + storedSize(u64) + rawSize(u64)
    //                + contentHash(u64).
    const auto tocRecordSize = [](usize pathLen) -> u64 {
        return sizeof(u32) + pathLen + 4 * sizeof(u64);
    };

    for (u32 p = 0; p < packCount; ++p) {
        const fs::path packFile = outDir / (baseName + "_" + std::to_string(p) + ".uap");

        // Lay out this chunk's TOC + blobs. Blobs are aligned; a blob whose RAW content
        // matches an EARLIER slot IN THIS SAME pack is DEDUPED - its TOC entry points at
        // the earlier blob and its bytes are written once. Dedup is confined to one pack
        // file so each pack stays self-contained and byte-identical across cooks (the
        // patchability contract - deleting an asset never perturbs another pack).
        struct SlotEntry {
            std::string path;
            u64 offset = 0;
            u64 storedSize = 0; // stored (possibly compressed) byte count
            u64 rawSize = 0;
            u64 hash = 0;
            bool deduped = false;    // shares an earlier slot's blob (bytes not re-written)
            const std::vector<u8>* bytes = nullptr; // owning blob (null for empty/deduped)
        };
        std::vector<SlotEntry> chunk(kSlotsPerPack);
        u64 tocSize = kHeaderSize;
        for (u32 s = 0; s < kSlotsPerPack; ++s) {
            SlotEntry& e = chunk[s];
            if (auto it = bySlot.find(p * kSlotsPerPack + s); it != bySlot.end()) {
                e.path = it->second;
                Blob& b = loaded[blobIndexBySlot.at(p * kSlotsPerPack + s)];
                e.storedSize = b.stored.size();
                e.rawSize = b.rawSize;
                e.hash = b.hash;
                e.bytes = &b.stored;
            }
            tocSize += tocRecordSize(e.path.size());
        }

        // Assign aligned offsets in slot order, deduping identical content within the
        // pack. `byHash` maps a content hash to the FIRST slot index that placed it.
        std::unordered_map<u64, u32> byHash;
        u64 cursor = AlignUp(tocSize, kBlobAlignment);
        for (u32 s = 0; s < kSlotsPerPack; ++s) {
            SlotEntry& e = chunk[s];
            if (e.path.empty()) continue;
            if (auto it = byHash.find(e.hash); it != byHash.end()) {
                const SlotEntry& first = chunk[it->second];
                // Hash match: confirm byte-identity before sharing (collision-safe).
                if (first.storedSize == e.storedSize && first.rawSize == e.rawSize &&
                    e.bytes && first.bytes && *e.bytes == *first.bytes) {
                    e.offset = first.offset;
                    e.deduped = true;
                    result.dedupedBytes += e.rawSize;
                    ++result.dedupedCount;
                    continue;
                }
            }
            e.offset = cursor;
            cursor += e.storedSize;
            cursor = AlignUp(cursor, kBlobAlignment);
            result.packedBytes += e.storedSize;
            byHash.emplace(e.hash, s);
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
        pod(static_cast<u32>(codec));
        pod(kBlobAlignment);
        for (const SlotEntry& e : chunk) {
            pod(static_cast<u32>(e.path.size()));
            if (!e.path.empty()) {
                std::string scrambled = e.path; // obfuscate on disk (v4+); reader decodes
                ScramblePath(scrambled);
                out.write(scrambled.data(), static_cast<std::streamsize>(scrambled.size()));
            }
            pod(e.offset);
            pod(e.storedSize);
            pod(e.rawSize);
            pod(e.hash);
        }
        // Blob region: write each NON-deduped blob at its aligned offset, padding the gap
        // from the current position. Offsets increase monotonically for non-deduped
        // blobs (assigned in slot order), so a single forward pass with zero-padding
        // reproduces the layout exactly - and pack bytes stay deterministic.
        u64 pos = tocSize;
        static const u8 kZero[kBlobAlignment] = {};
        for (const SlotEntry& e : chunk) {
            if (e.path.empty() || e.deduped) continue;
            if (e.offset > pos) {
                out.write(reinterpret_cast<const char*>(kZero),
                          static_cast<std::streamsize>(e.offset - pos));
                pos = e.offset;
            }
            out.write(reinterpret_cast<const char*>(e.bytes->data()),
                      static_cast<std::streamsize>(e.storedSize));
            pos += e.storedSize;
        }
    }

    // Remove pack files a PREVIOUS, larger cook left behind. PackSet::Open walks
    // `<base>_0`, `_1`, ... until one is missing, so a stale trailing chunk would
    // be mounted and could shadow a current entry by path - and with slots now
    // sparse rather than compacted, the pack count genuinely shrinks when the
    // high assets are deleted.
    for (u32 p = packCount;; ++p) {
        const fs::path stale = outDir / (baseName + "_" + std::to_string(p) + ".uap");
        if (!fs::exists(stale, ec)) break;
        // Stop on a failed delete rather than retrying the same path forever: a
        // locked file would otherwise spin here (`exists` stays true).
        if (!fs::remove(stale, ec) || ec) {
            HBE_WARN("UAP: could not remove the stale pack '{}'; delete it by hand or a "
                     "mount will still see its (outdated) entries.",
                     stale.string());
            break;
        }
    }

    HBE_INFO("UAP: packed {} assets into {} pack(s) of {} slots ('{}_N.uap'){}{}.",
             result.assetCount, result.packCount, kSlotsPerPack, baseName,
             options.compress
                 ? " with " + std::string(comp::ToString(codec)) + " at " +
                       std::to_string(result.rawBytes ? result.packedBytes * 100 /
                                                            result.rawBytes
                                                      : 100) +
                       "% of source size"
                 : "",
             result.dedupedCount
                 ? " (deduped " + std::to_string(result.dedupedCount) + " slot(s), saving " +
                       std::to_string(result.dedupedBytes / 1024) + " KB)"
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
    // The slot count is written into every pack, but nothing used to check it -
    // and PackSet::Entries() globalises with the COMPILE-TIME kSlotsPerPack
    // (`e.slot += p * kSlotsPerPack`). Changing the constant would therefore have
    // silently mis-numbered every already-shipped pack. Refuse to mount instead.
    if (count != kSlotsPerPack) {
        HBE_ERROR("UAP: '{}' was written with {} slots per pack but this build uses {}. "
                  "Re-cook the packs; mounting them would mis-number every slot.",
                  packFile.string(), count, kSlotsPerPack);
        return false;
    }
    u32 codecField = 0;
    if (version >= 3) pod(codecField); // v3/4: Windows algorithm; v5: portable comp::Codec
    u32 alignment = 1;
    if (version >= 5) pod(alignment); // recorded for validation / mmap consumers
    (void)alignment;
    codec_ = static_cast<u32>(ResolveCodec(version, codecField));
    // A pack we cannot decode (a legacy LZMS pack on a non-Windows build) must fail to
    // mount loudly rather than hand back garbage on the first Read.
    if (!comp::CanDecode(static_cast<comp::Codec>(codec_))) {
        HBE_ERROR("UAP: '{}' uses codec {} which this build cannot decode. Re-cook the "
                  "packs to get portable zstd.", packFile.string(),
                  comp::ToString(static_cast<comp::Codec>(codec_)));
        return false;
    }

    for (u32 i = 0; i < count; ++i) {
        u32 len = 0;
        pod(len);
        if (!in || len > 4096) return false;
        Entry e;
        e.slot = i;
        e.path.resize(len);
        if (len) in.read(e.path.data(), len);
        if (len && version >= 4) ScramblePath(e.path); // de-obfuscate v4+ paths
        pod(e.offset);
        pod(e.size);
        if (version >= 3) {
            pod(e.rawSize);
        } else {
            e.rawSize = e.size; // v2: always stored verbatim
        }
        if (version >= 5) pod(e.contentHash);
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
    auto raw = comp::Decompress(static_cast<comp::Codec>(codec_), bytes.data(), bytes.size(),
                                e.rawSize);
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

// ---------------------------------------------------------------------------
// Self-test (--test-uapv5)
// ---------------------------------------------------------------------------
namespace {

bool WriteFileBytes(const fs::path& p, const std::vector<u8>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!b.empty()) out.write(reinterpret_cast<const char*>(b.data()),
                              static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

std::vector<u8> ReadFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<u8> b(static_cast<usize>(n));
    if (n) in.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

// Hand-builds a minimal v4 (pre-portable) pack with ONE stored entry, to prove the v5
// reader still opens legacy packs. Stored (algorithm 0) so it needs no Windows LZMS.
bool WriteLegacyV4Pack(const fs::path& packFile, const std::string& path,
                       const std::vector<u8>& payload) {
    std::vector<u8> toc;
    const auto putU32 = [&](u32 v) {
        const u8* b = reinterpret_cast<const u8*>(&v);
        toc.insert(toc.end(), b, b + 4);
    };
    const auto putU64 = [&](u64 v) {
        const u8* b = reinterpret_cast<const u8*>(&v);
        toc.insert(toc.end(), b, b + 8);
    };
    // TOC size: header(16) + 50 * (u32 pathLen + path + 3*u64). Only slot 0 occupied.
    u64 tocSize = 16;
    for (u32 s = 0; s < kSlotsPerPack; ++s)
        tocSize += 4 + (s == 0 ? path.size() : 0) + 3 * 8;
    std::ofstream out(packFile, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(kMagic, 4);
    putU32(4);              // version
    putU32(kSlotsPerPack);  // slotCount
    putU32(0);              // algorithm 0 = stored
    for (u32 s = 0; s < kSlotsPerPack; ++s) {
        if (s == 0) {
            putU32(static_cast<u32>(path.size()));
            std::string scr = path;
            ScramblePath(scr); // v4 scrambles paths
            toc.insert(toc.end(), scr.begin(), scr.end());
            putU64(tocSize);                         // offset
            putU64(payload.size());                  // size (stored)
            putU64(payload.size());                  // rawSize == size
        } else {
            putU32(0);
            putU64(0);
            putU64(0);
            putU64(0);
        }
    }
    out.write(reinterpret_cast<const char*>(toc.data()),
              static_cast<std::streamsize>(toc.size()));
    if (!payload.empty())
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
    return static_cast<bool>(out);
}

} // namespace

bool PackSelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { HBE_ERROR("uapv5: FAIL - {}", what); ++fails; }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_uapv5_test";
    fs::remove_all(dir, ec);
    const fs::path assets = dir / "Assets";
    const fs::path outA = dir / "outA";
    const fs::path outB = dir / "outB";
    const fs::path manifest = dir / "test.uapmanifest";
    fs::create_directories(assets, ec);
    fs::create_directories(outA, ec);
    fs::create_directories(outB, ec);

    // A corpus with a byte-identical PAIR (dupe1/dupe2 -> within-pack dedup), a unique
    // compressible blob, a unique incompressible blob, and an empty file. `.uaf` so the
    // packable-extension test accepts them; the content is arbitrary (the cook never
    // parses it, and slots::ReadSlot treats a bad header as "no embedded id").
    std::vector<u8> dup(20000);
    for (usize i = 0; i < dup.size(); ++i) dup[i] = static_cast<u8>((i * 131 + 7) & 0xFF);
    std::vector<u8> compressible(40000, 0xCD);
    std::vector<u8> noise(30000);
    { u32 s = 99; for (u8& b : noise) { s = s * 1664525u + 1013904223u; b = static_cast<u8>(s >> 24); } }

    struct Item { const char* name; std::vector<u8> bytes; };
    std::vector<Item> items = {
        {"a_dupe1.uaf", dup},
        {"b_dupe2.uaf", dup}, // identical to a_dupe1 -> must dedup
        {"c_flat.uaf", compressible},
        {"d_noise.uaf", noise},
        {"e_empty.uaf", {}},
    };
    for (const Item& it : items)
        check(WriteFileBytes(assets / it.name, it.bytes), "write source asset");

    // Cook (compressed).
    WriteOptions opt;
    opt.compress = true;
    auto res = WritePacks(outA, "T", assets, manifest, opt);
    check(res.has_value(), "WritePacks must succeed");
    if (!res) { fs::remove_all(dir, ec); return false; }
    check(res->assetCount == items.size(), "asset count matches the corpus");
    check(res->dedupedCount >= 1, "the identical pair must be deduped");
    check(res->dedupedBytes >= dup.size(), "dedup savings must count the shared blob");

    // Read every asset back and byte-compare; check content hash + alignment.
    PackSet set;
    check(set.Open(outA, "T"), "PackSet must open the cooked packs");
    for (const Item& it : items) {
        auto got = set.Read(it.name);
        check(got.has_value(), "each asset must read back");
        if (got) check(*got == it.bytes, "round-trip bytes must match the source");
    }
    // Offsets aligned, and the stored hash matches the raw content.
    for (const Entry& e : set.Entries()) {
        check(e.offset % kBlobAlignment == 0, "every blob offset must be aligned");
        auto got = set.Read(e.path);
        if (got)
            check(e.contentHash == comp::Hash64(got->data(), got->size()),
                  "stored content hash must match the raw bytes");
    }

    // Determinism / patchability: a second cook (same manifest) is byte-identical.
    auto res2 = WritePacks(outB, "T", assets, manifest, opt);
    check(res2.has_value(), "second cook must succeed");
    if (res2) {
        for (u32 p = 0; p < res->packCount; ++p) {
            const auto a = ReadFileBytes(outA / ("T_" + std::to_string(p) + ".uap"));
            const auto b = ReadFileBytes(outB / ("T_" + std::to_string(p) + ".uap"));
            check(!a.empty() && a == b, "re-cook must produce byte-identical packs");
        }
    }

    // Legacy v4 (stored) pack must still open + read under the v5 reader.
    {
        const fs::path legacyDir = dir / "legacy";
        fs::create_directories(legacyDir, ec);
        std::vector<u8> payload(1234);
        for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<u8>(i & 0xFF);
        check(WriteLegacyV4Pack(legacyDir / "L_0.uap", "old/thing.uaf", payload),
              "hand-write a v4 pack");
        PackReader r;
        check(r.Open(legacyDir / "L_0.uap"), "v5 reader must open a v4 pack");
        auto got = r.Read("old/thing.uaf");
        check(got && *got == payload, "v4 stored entry must read back unchanged");
    }

    fs::remove_all(dir, ec);
    if (fails == 0)
        HBE_INFO("uapv5: passed (dedup, content hash, blob alignment, deterministic re-cook, "
                 "v4 back-compat).");
    return fails == 0;
}

} // namespace hbe::uap
