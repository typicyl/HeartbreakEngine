// Assets/SlotIds.cpp
#include "Assets/SlotIds.h"

#include "Assets/AssetFormats.h" // the single source of truth for what is packable
#include "Assets/UAF.h"          // the `.uaf` header layout + its slot flag
#include "Assets/UAP.h"          // kSlotsPerPack (the manifest records it)
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

namespace hbe::slots {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// The pack key for a file: Assets-relative, forward slashes. This is EXACTLY the
// string uap::WritePacks matches its filter against and stores in a pack TOC, so
// deriving it here (rather than approximating it) is what keeps the allocator,
// the cooker and the runtime talking about the same asset.
std::string PackKey(const fs::path& assetsDir, const fs::path& file) {
    std::error_code ec;
    const fs::path rel = fs::relative(file, assetsDir, ec);
    if (ec || rel.empty()) return file.filename().generic_string();
    return rel.generic_string();
}

// Is this pack key an asset under Assets/ at all? A manifest also remembers ids
// for the compiled shaders and `__project.hbproj`, which ship as uap::ExtraFile
// and are not this module's to judge.
bool IsAssetKey(const std::string& key) {
    return assets::IsPackable(assets::NormalizeExtension(std::filesystem::path(key)));
}

bool IsJsonAsset(const std::string& ext) {
    return assets::IsPackable(ext) && assets::RefScanOf(ext) == assets::RefScan::JsonScan;
}

u32 ReadLE32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The `.uaf` header, read from the front of a file. `hasSlot` reflects the flag
// bit on the version word; `version` is the payload version with that bit masked
// off (a stamped v5 asset is still a v5 asset).
struct UafHeader {
    u32 version = 0;
    u32 type = 0;
    u64 guid = 0;
    bool hasSlot = false;
    u32 slot = kUnassigned;
};

bool ParseUafHeader(const u8* bytes, usize size, UafHeader& out) {
    if (size < uaf::kHeaderSize) return false;
    if (std::memcmp(bytes, uaf::kMagic, 4) != 0) return false;
    const u32 raw = ReadLE32(bytes + 4);
    out.hasSlot = (raw & uaf::kSlotFlag) != 0;
    out.version = raw & ~uaf::kSlotFlag;
    if (out.version == 0 || out.version > uaf::kVersion) return false;
    out.type = ReadLE32(bytes + 8);
    std::memcpy(&out.guid, bytes + 12, sizeof(out.guid));
    if (out.hasSlot) {
        if (size < uaf::kHeaderSizeWithSlot) return false;
        out.slot = ReadLE32(bytes + uaf::kHeaderSize);
    }
    return true;
}

std::optional<u32> ReadUafSlot(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    u8 hdr[uaf::kHeaderSizeWithSlot] = {};
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    const usize got = static_cast<usize>(in.gcount());
    UafHeader h;
    if (!ParseUafHeader(hdr, got, h)) return std::nullopt;
    return h.hasSlot ? h.slot : kUnassigned;
}

bool WriteUafSlot(const fs::path& file, u32 slot) {
    std::error_code ec;
    // Fast path: the header already reserves the field (everything the current
    // importer writes does), so this is a 4-byte patch - no rewrite, however
    // large the payload.
    {
        std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
        if (!io) return false;
        u8 hdr[uaf::kHeaderSizeWithSlot] = {};
        io.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        const usize got = static_cast<usize>(io.gcount());
        UafHeader h;
        if (!ParseUafHeader(hdr, got, h)) return false;
        if (h.hasSlot) {
            io.clear();
            io.seekp(static_cast<std::streamoff>(uaf::kHeaderSize));
            io.write(reinterpret_cast<const char*>(&slot), sizeof(slot));
            return static_cast<bool>(io);
        }
    }

    // Slow path, once per legacy asset: the field has to be INSERTED. Streamed
    // through a sibling temp file and renamed over the original, so an
    // interrupted stamp leaves the asset untouched rather than truncated.
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    u8 hdr[uaf::kHeaderSize] = {};
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    UafHeader h;
    if (static_cast<usize>(in.gcount()) < uaf::kHeaderSize ||
        !ParseUafHeader(hdr, uaf::kHeaderSize, h)) {
        return false;
    }
    const fs::path tmp = fs::path(file).concat(".slottmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        // The version word gains the flag bit; the PAYLOAD version is unchanged,
        // so a v5 asset stays a v5 asset and its reader keeps skipping the v8
        // blendshape block it never had.
        const u32 flagged = h.version | uaf::kSlotFlag;
        out.write(reinterpret_cast<const char*>(uaf::kMagic), 4);
        out.write(reinterpret_cast<const char*>(&flagged), sizeof(flagged));
        out.write(reinterpret_cast<const char*>(&h.type), sizeof(h.type));
        out.write(reinterpret_cast<const char*>(&h.guid), sizeof(h.guid));
        out.write(reinterpret_cast<const char*>(&slot), sizeof(slot));
        out << in.rdbuf(); // the payload, verbatim
        if (!out) return false;
    }
    in.close();
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<u32> ReadJsonSlot(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    try {
        json j;
        in >> j;
        if (!j.is_object()) return std::nullopt;
        const auto it = j.find("packSlot");
        if (it == j.end() || !it->is_number_unsigned()) return kUnassigned;
        return it->get<u32>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool WriteJsonSlot(const fs::path& file, u32 slot) {
    json j;
    {
        std::ifstream in(file);
        if (!in) return false;
        try {
            in >> j;
        } catch (const std::exception&) {
            return false;
        }
    }
    if (!j.is_object()) return false;
    j["packSlot"] = slot;
    // TEMP FILE + RENAME, exactly like WriteUafSlot. `std::ofstream(file,
    // std::ios::trunc)` empties the author's scene/material/schematic BEFORE the
    // first byte is written, so an AV lock, a full disk, or the editor holding the
    // handle turns a one-integer stamp into a lost asset - and --migrate-slots
    // --apply runs this over every JSON asset in the project in one go, with no
    // rollback. Renaming over the original is atomic on NTFS: the file is either
    // the old one or the new one, never a truncated one.
    std::error_code ec;
    const fs::path tmp = fs::path(file).concat(".slottmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Every packable file under `assetsDir`, as pack keys, sorted. Sorted order is
// what makes "lowest free slot" a deterministic function of the file set rather
// than of directory-iteration order.
std::vector<std::string> PackableKeys(const fs::path& assetsDir) {
    std::vector<std::string> keys;
    std::error_code ec;
    if (!fs::exists(assetsDir, ec)) return keys;
    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break; // a WALK failure is fatal - a truncated scan would hand out
                       // ids that are already in use further down the tree
        std::error_code fec; // separate: a per-ENTRY failure must not look like one
        if (!it->is_regular_file(fec) || fec) continue;
        if (!assets::IsPackable(assets::NormalizeExtension(it->path()))) continue;
        keys.push_back(PackKey(assetsDir, it->path()));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace

// --- Format dispatch --------------------------------------------------------

bool CanEmbedSlot(const std::string& ext) {
    // Derived from the ONE registry, never a second hand-written list: a format
    // can hold an id iff it ships AND it is either the `.uaf` container (whose
    // header reserves a field) or a JSON document (which takes a top-level key).
    // Everything else - the binary bakes - uses the manifest, tier 2.
    if (!assets::IsPackable(ext)) return false;
    return ext == ".uaf" || IsJsonAsset(ext);
}

std::optional<u32> ReadSlot(const fs::path& file) {
    const std::string ext = assets::NormalizeExtension(file);
    if (!assets::IsPackable(ext)) return std::nullopt;
    if (!CanEmbedSlot(ext)) return kUnassigned; // tier 2 carries it; not an error
    if (ext == ".uaf") return ReadUafSlot(file);
    return ReadJsonSlot(file);
}

bool WriteSlot(const fs::path& file, u32 slot) {
    if (slot == kUnassigned) return false;
    const std::string ext = assets::NormalizeExtension(file);
    if (!CanEmbedSlot(ext)) return false;
    if (ext == ".uaf") return WriteUafSlot(file, slot);
    return WriteJsonSlot(file, slot);
}

// --- SlotTable --------------------------------------------------------------

void SlotTable::Clear() {
    byKey_.clear();
    used_.clear();
    nextFreeHint_ = 0;
}

bool SlotTable::Claim(const std::string& key, u32 slot) {
    if (slot == kUnassigned) return false;
    if (const auto it = byKey_.find(key); it != byKey_.end()) return it->second == slot;
    if (used_.count(slot)) return false;
    byKey_[key] = slot;
    used_.insert(slot);
    return true;
}

u32 SlotTable::ClaimLowestFree(const std::string& key) {
    if (const auto it = byKey_.find(key); it != byKey_.end()) return it->second;
    const u32 slot = LowestFree();
    byKey_[key] = slot;
    used_.insert(slot);
    nextFreeHint_ = slot + 1;
    return slot;
}

void SlotTable::Release(const std::string& key) {
    const auto it = byKey_.find(key);
    if (it == byKey_.end()) return;
    used_.erase(it->second);
    nextFreeHint_ = std::min(nextFreeHint_, it->second);
    byKey_.erase(it);
}

u32 SlotTable::SlotOf(const std::string& key) const {
    const auto it = byKey_.find(key);
    return it == byKey_.end() ? kUnassigned : it->second;
}

bool SlotTable::InUse(u32 slot) const { return used_.count(slot) != 0; }

u32 SlotTable::LowestFree() const {
    // THE rule the whole feature is built on, and it is deliberately the same
    // line the cooker has always run: scan up from zero and take the first hole.
    // A deleted asset leaves a hole; the next created asset falls into it.
    u32 slot = nextFreeHint_;
    while (used_.count(slot)) ++slot;
    return slot;
}

// --- Manifest (tier 2) ------------------------------------------------------

std::map<std::string, u32> LoadRememberedSlots(const fs::path& manifestPath) {
    std::map<std::string, u32> slots;
    std::ifstream in(manifestPath);
    if (!in) return slots;
    try {
        json j;
        in >> j;
        // NOTE: don't call .items() on a j.value(...) temporary - the proxy would
        // outlive it (C++20 range-for does not extend that lifetime).
        if (const auto it = j.find("slots"); it != j.end() && it->is_object()) {
            for (const auto& [path, slot] : it->items()) {
                if (slot.is_number_unsigned()) slots[path] = slot.get<u32>();
            }
        }
    } catch (const std::exception&) {
        HBE_WARN("slots: manifest '{}' is corrupt; only ids embedded in the assets "
                 "themselves will be honoured this cook.",
                 manifestPath.string());
        slots.clear();
    }
    return slots;
}

bool SaveRememberedSlots(const fs::path& manifestPath, const std::map<std::string, u32>& slots) {
    // Sorted by slot so diffs of the manifest stay readable. A duplicate slot
    // would silently drop a path here, so it is refused instead - the caller's
    // allocator is what guarantees uniqueness, and a violation is a real bug.
    std::map<u32, std::string> bySlot;
    for (const auto& [path, slot] : slots) {
        if (const auto it = bySlot.find(slot); it != bySlot.end()) {
            HBE_ERROR("slots: '{}' and '{}' both claim slot {} - refusing to write a "
                      "manifest that would silently forget one.",
                      it->second, path, slot);
            return false;
        }
        bySlot[slot] = path;
    }
    json j;
    // v2: the manifest is no longer the AUTHORITY, it is the remembered fallback
    // for files that cannot carry an id (see SlotIds.h). v1 files read identically.
    j["version"] = 2;
    j["slotsPerPack"] = uap::kSlotsPerPack;
    auto& out = j["slots"] = json::object();
    for (const auto& [slot, path] : bySlot) out[path] = slot;

    std::error_code ec;
    if (!manifestPath.parent_path().empty()) fs::create_directories(manifestPath.parent_path(), ec);
    // Write atomically (temp + rename), like the embedded-slot writers: a crash or disk-full while
    // truncating in place would leave a corrupt manifest, and LoadRememberedSlots treats a corrupt
    // manifest as EMPTY - which (for any format that can't embed its id) silently forgets every
    // remembered slot and renumbers the packs on the next cook.
    fs::path tmp = manifestPath;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file) return false;
        file << j.dump(2);
        if (!file) {
            file.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, manifestPath, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// --- Stamping ---------------------------------------------------------------

fs::file_time_type MarkNow() {
    // Five seconds of slack: file timestamps and the file clock do not tick
    // together, and a filesystem may round. Over-including here only means
    // re-examining a file that already has an id, which writes nothing.
    return fs::file_time_type::clock::now() - std::chrono::seconds(5);
}

namespace {

// Every stamp rebuilds the allocator from the whole project, and a folder drop
// runs one stamp PER IMPORTED FILE - so on a large project the same hundreds of
// files (including multi-megabyte `.hbscene` documents that have to be parsed as
// JSON) would be re-read once per dropped file. This caches the answer against
// the file's write time AND size, so a file that actually changed is always
// re-read and a file that did not is free. Editor-lifetime; a mutex because
// WritePacks may be reached from a job thread.
std::optional<u32> ReadSlotCached(const fs::path& file) {
    struct Cached {
        fs::file_time_type when{};
        u64 size = 0;
        std::optional<u32> slot;
    };
    static std::map<std::string, Cached> cache;
    static std::mutex mutex;

    std::error_code ec;
    const auto when = fs::last_write_time(file, ec);
    if (ec) return ReadSlot(file);
    const u64 size = static_cast<u64>(fs::file_size(file, ec));
    if (ec) return ReadSlot(file);

    const std::string key = file.generic_string();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(key);
            it != cache.end() && it->second.when == when && it->second.size == size) {
            return it->second.slot;
        }
    }
    const std::optional<u32> slot = ReadSlot(file);
    const std::lock_guard<std::mutex> lock(mutex);
    cache[key] = Cached{when, size, slot};
    return slot;
}

// Builds the allocator from the CURRENT truth on disk plus the remembered map:
// every packable file's embedded id is reserved, then every remembered id whose
// file still exists. A remembered id whose file is gone is dropped - that is what
// frees a deleted asset's number for the next import.
void SeedTable(const fs::path& assetsDir, const std::map<std::string, u32>& remembered,
               SlotTable& table, std::vector<std::string>& unstamped) {
    std::error_code ec;
    const std::vector<std::string> keys = PackableKeys(assetsDir);
    for (const std::string& key : keys) {
        const std::optional<u32> embedded = ReadSlotCached(assetsDir / fs::path(key));
        if (embedded && *embedded != kUnassigned) {
            if (!table.Claim(key, *embedded)) {
                HBE_WARN("slots: '{}' claims slot {}, which is already taken; it will be "
                         "re-numbered.",
                         key, *embedded);
                unstamped.push_back(key);
            }
            continue;
        }
        unstamped.push_back(key);
    }
    // NON-ASSETS FIRST, in their own pass. The compiled shaders and
    // `__project.hbproj` ship as uap::ExtraFile in the same slot space, nothing here
    // can check whether they still exist, and a displaced shader invalidates a pack
    // for every player exactly like a displaced asset does. Reserving them ahead of
    // the asset pass means the two can never race for a number just because one of
    // them happened to sort earlier in the map.
    for (const auto& [key, slot] : remembered)
        if (!IsAssetKey(key)) table.Claim(key, slot);
    // Tier 2. An asset's remembered id is honoured ONLY while the asset still
    // exists - that is precisely what frees a deleted asset's number for the next
    // creation.
    for (const auto& [key, slot] : remembered) {
        if (!IsAssetKey(key)) continue;
        if (table.SlotOf(key) != kUnassigned) continue;
        if (!fs::exists(assetsDir / fs::path(key), ec)) continue;
        table.Claim(key, slot);
    }
}

} // namespace

bool IsAssetSlotKey(const std::string& key) { return IsAssetKey(key); }

void MergeRememberedSlots(const fs::path& assetsDir, const SlotTable& table,
                          std::map<std::string, u32>& remembered) {
    std::error_code ec;
    for (auto it = remembered.begin(); it != remembered.end();) {
        const bool gone = IsAssetKey(it->first) && !fs::exists(assetsDir / fs::path(it->first), ec);
        it = gone ? remembered.erase(it) : std::next(it);
    }
    for (const auto& [key, slot] : table.All()) remembered[key] = slot;
}

namespace {

// Local alias so the call sites below read as they always did.
void MergeMemory(const fs::path& assetsDir, const SlotTable& table,
                 std::map<std::string, u32>& remembered) {
    MergeRememberedSlots(assetsDir, table, remembered);
}

} // namespace

u32 StampAsset(const fs::path& assetsDir, const fs::path& manifestPath, const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return kUnassigned;
    const std::string ext = assets::NormalizeExtension(file);
    if (!assets::IsPackable(ext)) return kUnassigned;
    const std::string key = PackKey(assetsDir, file);

    if (const std::optional<u32> have = ReadSlot(file); have && *have != kUnassigned)
        return *have; // already has one; nothing to do

    std::map<std::string, u32> remembered = LoadRememberedSlots(manifestPath);
    SlotTable table;
    std::vector<std::string> unstamped;
    SeedTable(assetsDir, remembered, table, unstamped);

    u32 slot = table.SlotOf(key);
    if (slot == kUnassigned) {
        // Prefer what the manifest remembers for this exact key. That is the
        // re-import case: overwriting `wood.uaf` clears its header field, and
        // handing it a fresh number would reshuffle the packs for a gamma fix.
        const auto it = remembered.find(key);
        if (it != remembered.end() && table.Claim(key, it->second)) slot = it->second;
        else slot = table.ClaimLowestFree(key);
    }

    if (CanEmbedSlot(ext) && !WriteSlot(file, slot)) {
        // ERROR, not WARN: the id is the asset's pack address for life, and a file
        // that would not take one is back on path-keyed memory - a later rename
        // then reads as delete + create and moves it to a different pack. The file
        // itself is intact (the write is temp-file + rename), but say which one.
        HBE_ERROR("slots: could not stamp pack id {} into '{}' - the file was NOT modified, "
                  "but it now depends on the manifest alone to keep that number.",
                  slot, key);
    }
    MergeMemory(assetsDir, table, remembered);
    SaveRememberedSlots(manifestPath, remembered);
    return slot;
}

u32 StampNewAssets(const fs::path& assetsDir, const fs::path& manifestPath,
                   fs::file_time_type since) {
    std::error_code ec;
    if (!fs::exists(assetsDir, ec)) return 0;
    std::map<std::string, u32> remembered = LoadRememberedSlots(manifestPath);
    SlotTable table;
    std::vector<std::string> unstamped;
    SeedTable(assetsDir, remembered, table, unstamped);

    u32 stamped = 0;
    bool dirty = false;
    for (const std::string& key : unstamped) { // already sorted by PackableKeys
        const fs::path file = assetsDir / fs::path(key);
        const auto when = fs::last_write_time(file, ec);
        if (ec || when < since) continue; // untouched by this operation - leave it alone
        u32 slot = table.SlotOf(key);
        if (slot == kUnassigned) {
            const auto it = remembered.find(key);
            if (it != remembered.end() && table.Claim(key, it->second)) slot = it->second;
            else slot = table.ClaimLowestFree(key);
        }
        if (CanEmbedSlot(assets::NormalizeExtension(file))) {
            if (WriteSlot(file, slot)) ++stamped;
            else
                HBE_ERROR("slots: could not stamp pack id {} into '{}' - the file was NOT "
                          "modified, but it now depends on the manifest alone to keep that "
                          "number.",
                          slot, key);
        }
        dirty = true;
    }
    if (dirty) {
        MergeMemory(assetsDir, table, remembered);
        SaveRememberedSlots(manifestPath, remembered);
    }
    return stamped;
}

u32 RekeyAsset(const fs::path& assetsDir, const fs::path& manifestPath, const fs::path& from,
               const fs::path& to) {
    const std::string oldKey = PackKey(assetsDir, from);
    const std::string newKey = PackKey(assetsDir, to);
    if (oldKey.empty() || newKey.empty() || oldKey == newKey) return 0;

    std::map<std::string, u32> remembered = LoadRememberedSlots(manifestPath);
    if (remembered.empty()) return 0;

    // A file re-keys exactly itself; a directory re-keys everything beneath it.
    // The "/" is what stops `Paint` from also matching `Painterly/...`.
    const std::string oldPrefix = oldKey + "/";
    const std::string newPrefix = newKey + "/";
    std::map<std::string, u32> moved;
    u32 n = 0;
    for (auto it = remembered.begin(); it != remembered.end();) {
        std::string dst;
        if (it->first == oldKey) {
            dst = newKey;
        } else if (it->first.rfind(oldPrefix, 0) == 0) {
            dst = newPrefix + it->first.substr(oldPrefix.size());
        }
        if (dst.empty()) {
            ++it;
            continue;
        }
        moved[dst] = it->second;
        it = remembered.erase(it);
        ++n;
    }
    if (n == 0) return 0;
    // A destination that somehow already had an entry keeps the MOVED asset's id -
    // the file at that path is now the moved one, so its number is the true one.
    for (const auto& [key, slot] : moved) remembered[key] = slot;
    if (!SaveRememberedSlots(manifestPath, remembered)) {
        HBE_ERROR("slots: '{}' -> '{}' could not be recorded in '{}'; those assets will be "
                  "renumbered at the next cook, which rewrites the packs they live in.",
                  oldKey, newKey, manifestPath.string());
        return 0;
    }
    return n;
}

// --- Migration --------------------------------------------------------------

MigrateStats MigrateSlotIds(const fs::path& assetsDir, const fs::path& manifestPath,
                            const fs::path& seedManifest, bool dryRun) {
    MigrateStats st;
    std::error_code ec;
    if (!fs::exists(assetsDir, ec)) {
        HBE_ERROR("migrate-slots: '{}' does not exist.", assetsDir.string());
        st.failed = 1;
        return st;
    }

    const std::map<std::string, u32> seed =
        seedManifest.empty() ? std::map<std::string, u32>{} : LoadRememberedSlots(seedManifest);
    std::map<std::string, u32> remembered = LoadRememberedSlots(manifestPath);

    const std::vector<std::string> keys = PackableKeys(assetsDir);
    st.scanned = static_cast<u32>(keys.size());

    // Pass 1 - honour what is already embedded. These never move: the whole point
    // of the id living in the file is that a re-run computes the same answer.
    SlotTable table;
    std::vector<std::string> needsId;
    for (const std::string& key : keys) {
        const std::optional<u32> embedded = ReadSlot(assetsDir / fs::path(key));
        if (!embedded) {
            HBE_ERROR("migrate-slots: cannot read '{}' - it is not a valid file for its "
                      "format. Fix or remove it; assigning it an id would hide the problem.",
                      key);
            ++st.failed;
            continue;
        }
        if (*embedded != kUnassigned) {
            if (table.Claim(key, *embedded)) {
                ++st.already;
                continue;
            }
            std::string holder = "(unknown)";
            for (const auto& [k, s] : table.All())
                if (s == *embedded) { holder = k; break; }
            HBE_WARN("migrate-slots: '{}' claims id {}, already held by '{}'. The "
                     "lexicographically lower path keeps it; this one is re-numbered.",
                     key, *embedded, holder);
            ++st.collisions;
        }
        needsId.push_back(key);
    }

    // Reserve the ids of everything that is NOT an asset under Assets/ - the
    // compiled shaders and `__project.hbproj` a shipping manifest remembers. They
    // ship in the same slot space, so handing one of their numbers to an asset
    // would collide at the next cook.
    //
    // FROM BOTH MAPS, seed first. Reserving only from `remembered` was a real hole:
    // when the seed and the target were separate files, ALL of the ExtraFile
    // reservations lived in the seed (measured: 107 in the ship manifest, 0 in the
    // dev one), so pass 3 below would hand an asset a shader's number and stamp it
    // permanently into the file. Harmless-looking, because it only fires for an
    // asset the seed has never seen - i.e. the first new asset after a migration.
    for (const auto& [key, slot] : seed)
        if (!IsAssetKey(key)) table.Claim(key, slot);
    for (const auto& [key, slot] : remembered)
        if (!IsAssetKey(key)) table.Claim(key, slot);
    // ...and persist them, so the editor-side allocator (which seeds from the
    // target manifest) inherits the same reservations instead of rediscovering the
    // same hole on every subsequent import.
    for (const auto& [key, slot] : table.All())
        if (!IsAssetKey(key)) remembered[key] = slot;

    // Pass 2 - seed from the manifest that describes the CURRENTLY SHIPPED layout,
    // so the first cook after the migration moves as few packs as possible; then
    // from this project's own manifest, which is where the binary bakes (that can
    // hold no field) keep their ids and is what makes a re-run a no-op for them.
    std::vector<std::string> stillFree;
    for (const std::string& key : needsId) {
        const auto sit = seed.find(key);
        if (sit != seed.end() && table.Claim(key, sit->second)) continue;
        const auto rit = remembered.find(key);
        if (rit != remembered.end() && table.Claim(key, rit->second)) continue;
        stillFree.push_back(key);
    }
    // Pass 3 - everything left, lowest free id in sorted-path order.
    for (const std::string& key : stillFree) table.ClaimLowestFree(key);

    // Report + apply.
    for (const std::string& key : needsId) {
        const u32 slot = table.SlotOf(key);
        const auto sit = seed.find(key);
        const bool seeded = sit != seed.end() && sit->second == slot;
        if (seeded) ++st.seeded;
        st.plan.emplace_back(key, slot, seeded);
        const fs::path file = assetsDir / fs::path(key);
        if (!CanEmbedSlot(assets::NormalizeExtension(file))) {
            // Binary bakes: the manifest is their home (tier 2). Counted, not failed.
            ++st.cannotEmbed;
            continue;
        }
        ++st.stamped;
        if (dryRun) continue;
        if (!WriteSlot(file, slot)) {
            HBE_ERROR("migrate-slots: FAILED to stamp id {} into '{}'.", slot, key);
            --st.stamped;
            ++st.failed;
        }
    }
    // Keep the memory of everything, embedded or not, so a cook agrees with this
    // run; assets that no longer exist lose their entry, which frees their id.
    MergeMemory(assetsDir, table, remembered);
    std::sort(st.plan.begin(), st.plan.end(),
              [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });

    if (!dryRun && !SaveRememberedSlots(manifestPath, remembered)) {
        HBE_ERROR("migrate-slots: could not write '{}'.", manifestPath.string());
        ++st.failed;
    }
    return st;
}

} // namespace hbe::slots
