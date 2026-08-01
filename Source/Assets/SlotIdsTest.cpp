// Assets/SlotIdsTest.cpp - --test-slotids.
//
// THE GATE for "an asset owns its pack slot for life". Every claim the feature
// makes is checked against a real scratch project on disk and a real cook:
//
//   1. A CREATED ASSET GETS THE NEXT ID, and the id is really inside the file -
//      re-read from disk, not from the allocator that just handed it out.
//   2. DELETING FREES, AND THE NEXT CREATION REUSES EXACTLY THAT NUMBER. Not
//      "some free number": the specific hole the deletion left.
//   3. NOTHING ELSE MOVES while that happens. The ids of every surviving asset
//      are compared one by one, before and after. This is the property the old
//      cooker did not have - it re-derived dense slots 0..N-1 in sorted-path
//      order, so adding `Audio/aaa.uaf` shifted every asset after it.
//   4. SLOT/50 PICKS THE PACK, checked at the boundary that actually matters:
//      49 is the last entry of pack 0, 50 the first of pack 1, 51 the second.
//      Proven by opening the cooked `.uap` files, not by arithmetic.
//   5. THE MIGRATION IS A NO-OP ON A SECOND RUN. It has to be: it writes into a
//      real project, and an operator will run it twice.
//   6. TWO COOKS IN A ROW PRODUCE THE SAME LAYOUT - identical pack count,
//      identical slot for every entry, and byte-identical pack files. A cook is
//      a pure function of the files on disk, which is the whole point.
//
// Plus the things that quietly break it: a `.uaf` written before slots existed
// (its header has no field to patch), a re-import that overwrites an asset and
// clears its header, a JSON saver that rebuilds its document and drops the key,
// and an asset MOVED to another folder - which under the old path-keyed manifest
// was indistinguishable from delete + create.
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
#include "Assets/SlotIds.h"

#include "Assets/AssetFormats.h"
#include "Assets/MaterialAsset.h"
#include "Assets/UAF.h"
#include "Assets/UAP.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hbe::slots {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

u32 g_failures = 0;

bool Check(bool cond, const std::string& what) {
    if (!cond) {
        HBE_ERROR("slotids: FAIL - {}", what);
        ++g_failures;
    }
    return cond;
}

bool CheckEq(u32 got, u32 want, const std::string& what) {
    return Check(got == want,
                 what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

// --- scratch project --------------------------------------------------------

struct Scratch {
    fs::path root, assets, manifest, packDir;
};

Scratch MakeScratch(const char* name) {
    std::error_code ec;
    Scratch s;
    s.root = fs::temp_directory_path(ec) / name;
    fs::remove_all(s.root, ec);
    s.assets = s.root / "Assets";
    s.packDir = s.root / "Pack";
    s.manifest = s.root / "Test.uapmanifest";
    fs::create_directories(s.assets, ec);
    fs::create_directories(s.packDir, ec);
    return s;
}

void WriteTex(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    uaf::Texture t;
    t.width = t.height = 2;
    t.format = 1;
    t.mipCount = 1;
    t.pixels.assign(2 * 2 * 4, 0xAAu);
    uaf::WriteTexture(path, t, 0x1234u);
}

void WriteMat(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    MaterialAsset m; // hbe::MaterialAsset; only its saver lives in hbe::assets
    m.name = path.stem().string();
    assets::SaveMaterial(path, m);
}

// A `.uaf` exactly as the engine wrote them BEFORE slots existed: a 20-byte
// header with no flag bit and no field to patch. Stamping one has to insert the
// field, which is the only path in the feature that rewrites a whole file.
void WriteLegacyUaf(const fs::path& path, u32 payloadVersion) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const u32 type = static_cast<u32>(uaf::AssetType::Texture);
    const u64 guid = 0xDEADBEEFCAFEBABEull;
    out.write(uaf::kMagic, 4);
    out.write(reinterpret_cast<const char*>(&payloadVersion), 4);
    out.write(reinterpret_cast<const char*>(&type), 4);
    out.write(reinterpret_cast<const char*>(&guid), 8);
    const char payload[] = "legacy-payload-bytes-that-must-survive";
    out.write(payload, sizeof(payload));
}

bool LegacyPayloadIntact(const fs::path& path, u32 wantVersion) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4] = {};
    in.read(magic, 4);
    u32 raw = 0, type = 0, slot = 0;
    u64 guid = 0;
    in.read(reinterpret_cast<char*>(&raw), 4);
    in.read(reinterpret_cast<char*>(&type), 4);
    in.read(reinterpret_cast<char*>(&guid), 8);
    in.read(reinterpret_cast<char*>(&slot), 4);
    if (std::memcmp(magic, uaf::kMagic, 4) != 0) return false;
    if ((raw & uaf::kSlotFlag) == 0) return false;
    if ((raw & ~uaf::kSlotFlag) != wantVersion) return false; // payload version UNCHANGED
    if (guid != 0xDEADBEEFCAFEBABEull) return false;
    char payload[64] = {};
    in.read(payload, 39);
    return std::string(payload) == "legacy-payload-bytes-that-must-survive";
}

// The id as it is on DISK. Every assertion goes through this rather than through
// the allocator, so "the number is stored with the asset" is what is proven.
u32 OnDisk(const Scratch& s, const std::string& key) {
    const auto v = ReadSlot(s.assets / fs::path(key));
    return v ? *v : kUnassigned;
}

// "Create an asset" the way the editor does: write the file, then stamp it.
u32 CreateAsset(const Scratch& s, const std::string& key) {
    const fs::path p = s.assets / fs::path(key);
    if (assets::NormalizeExtension(p) == ".uaf") WriteTex(p);
    else WriteMat(p);
    return StampAsset(s.assets, s.manifest, p);
}

void DeleteAsset(const Scratch& s, const std::string& key) {
    std::error_code ec;
    fs::remove(s.assets / fs::path(key), ec);
}

// Cooks and returns key -> global slot, read back out of the pack TOCs.
std::map<std::string, u32> Cook(const Scratch& s, const fs::path& outDir,
                                u32* outPackCount = nullptr) {
    std::map<std::string, u32> got;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    uap::WriteOptions wo;
    wo.compress = false;
    if (!uap::WritePacks(outDir, "Test", s.assets, s.manifest, wo)) return got;
    uap::PackSet set;
    if (!set.Open(outDir, "Test")) return got;
    if (outPackCount) *outPackCount = set.PackCount();
    for (const uap::Entry& e : set.Entries()) got[e.path] = e.slot;
    return got;
}

std::vector<u8> ReadBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize n = in.tellg();
    in.seekg(0);
    std::vector<u8> b(static_cast<usize>(n));
    in.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

// --- the cases --------------------------------------------------------------

// 1-3: import order, deletion freeing a hole, and nothing else moving.
void TestAllocationLifecycle() {
    const Scratch s = MakeScratch("hbe_slotids_alloc");

    // Created in this order; each must take the next number. Names are chosen so
    // sorted-path order is NOT creation order - under the old cooker the slots
    // would have followed the alphabet, which is exactly the bug.
    const std::vector<std::string> created = {"Zed/first.uaf", "Alpha/second.uaf",
                                              "Mid/third.hbmat", "Alpha/fourth.uaf",
                                              "Zed/fifth.hbmat"};
    for (u32 i = 0; i < created.size(); ++i) {
        CheckEq(CreateAsset(s, created[i]), i,
                "'" + created[i] + "' should have taken the next free id");
        CheckEq(OnDisk(s, created[i]), i,
                "'" + created[i] + "' does not carry its id ON DISK");
    }

    // Snapshot every id, then delete one from the middle.
    std::map<std::string, u32> before;
    for (const std::string& k : created) before[k] = OnDisk(s, k);
    DeleteAsset(s, "Mid/third.hbmat"); // held id 2

    // The next creation takes EXACTLY the freed number, not merely a free one.
    CheckEq(CreateAsset(s, "New/after_delete.uaf"), 2,
            "the next asset created after a deletion must reuse the freed id");

    // ...and no survivor moved.
    for (const auto& [key, slot] : before) {
        if (key == "Mid/third.hbmat") continue;
        CheckEq(OnDisk(s, key), slot, "'" + key + "' moved when another asset was deleted");
    }

    // Two more creations continue past the high-water mark, never re-using a
    // live id.
    CheckEq(CreateAsset(s, "New/sixth.uaf"), 5, "creation after the hole is refilled");
    CheckEq(CreateAsset(s, "New/seventh.hbmat"), 6, "creation continues from the high mark");

    // MOVING an asset carries its id. Under the old path-keyed manifest this was
    // delete + create, so a folder drag renumbered the asset and rewrote its pack.
    {
        std::error_code ec;
        const u32 was = OnDisk(s, "Alpha/second.uaf");
        fs::create_directories(s.assets / "Moved", ec);
        fs::rename(s.assets / "Alpha/second.uaf", s.assets / "Moved/second.uaf", ec);
        CheckEq(OnDisk(s, "Moved/second.uaf"), was,
                "an asset that was MOVED did not carry its id with it");
    }

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

// The three ways a stamped id can be lost, and what has to happen next.
void TestPreservation() {
    const Scratch s = MakeScratch("hbe_slotids_preserve");

    CreateAsset(s, "A/a.uaf");         // 0
    CreateAsset(s, "B/b.hbmat");       // 1
    const u32 cSlot = CreateAsset(s, "C/c.uaf"); // 2
    CheckEq(cSlot, 2, "third asset should hold id 2");

    // (a) RE-IMPORT: the file is overwritten, which clears the header field. The
    //     next stamp must give it back the SAME number - re-importing a texture
    //     to fix its gamma must not reshuffle the packs.
    WriteTex(s.assets / "C/c.uaf"); // overwrite, as importer::Import does
    CheckEq(OnDisk(s, "C/c.uaf"), kUnassigned, "an overwritten .uaf should have lost its id");
    CheckEq(StampAsset(s.assets, s.manifest, s.assets / "C/c.uaf"), cSlot,
            "a RE-IMPORTED asset was given a different id");
    CheckEq(OnDisk(s, "C/c.uaf"), cSlot, "the restored id was not written back to disk");

    // (b) A JSON SAVER that rebuilds its document from scratch drops the key.
    //     The manifest remembers, so the cook still packs it at the same slot -
    //     and a re-stamp puts it back in the file.
    WriteMat(s.assets / "B/b.hbmat");
    CheckEq(OnDisk(s, "B/b.hbmat"), kUnassigned, "a rewritten .hbmat should have lost its id");
    {
        const std::map<std::string, u32> cooked = Cook(s, s.packDir);
        Check(cooked.count("B/b.hbmat") != 0, "the rewritten .hbmat did not get packed");
        CheckEq(cooked.count("B/b.hbmat") ? cooked.at("B/b.hbmat") : kUnassigned, 1,
                "an asset whose saver dropped its id did not keep its slot via the manifest");
    }
    CheckEq(StampAsset(s.assets, s.manifest, s.assets / "B/b.hbmat"), 1,
            "re-stamping after a saver dropped the key changed the id");

    // (c) A LEGACY `.uaf` with no field at all: stamping must INSERT it, keep the
    //     payload version (a v5 asset must not start claiming to be a v8 asset,
    //     or its reader goes looking for blendshapes that are not there), and
    //     leave the payload byte-identical.
    WriteLegacyUaf(s.assets / "Legacy/old.uaf", 5);
    CheckEq(OnDisk(s, "Legacy/old.uaf"), kUnassigned, "a legacy .uaf should read as unassigned");
    const u32 legacySlot = StampAsset(s.assets, s.manifest, s.assets / "Legacy/old.uaf");
    CheckEq(legacySlot, 3, "the legacy asset should have taken the next free id");
    CheckEq(OnDisk(s, "Legacy/old.uaf"), 3, "the legacy .uaf was not stamped");
    Check(LegacyPayloadIntact(s.assets / "Legacy/old.uaf", 5),
          "stamping a legacy .uaf changed its payload version, guid or bytes");
    Check(uaf::PeekType(s.assets / "Legacy/old.uaf") == uaf::AssetType::Texture,
          "a stamped legacy .uaf no longer peeks as its own type");

    // A stamped asset still LOADS. The flag bit rides on the version word, so a
    // reader that forgot to mask it would reject every stamped asset in the
    // project - which would be the loudest possible failure, and is worth pinning.
    Check(uaf::ReadTexture(s.assets / "A/a.uaf").has_value(),
          "a stamped .uaf no longer reads back as a texture");
    Check(uaf::PeekType(s.assets / "A/a.uaf") == uaf::AssetType::Texture,
          "PeekType does not recognise a stamped .uaf");

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

// 4: slot / 50 selects the pack, checked at the boundary, against real packs.
void TestPackBoundary() {
    const Scratch s = MakeScratch("hbe_slotids_boundary");
    // 52 assets, created in an order that is deliberately not sorted order.
    std::vector<std::string> keys;
    for (u32 i = 0; i < 52; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Bulk/a%02u.uaf", 51u - i);
        keys.push_back(buf);
        CheckEq(CreateAsset(s, keys.back()), i, "bulk creation should hand out ids in order");
    }
    u32 packCount = 0;
    const std::map<std::string, u32> cooked = Cook(s, s.packDir, &packCount);
    CheckEq(packCount, 2, "52 assets at 50 slots per pack must produce 2 packs");
    CheckEq(static_cast<u32>(cooked.size()), 52, "the cook lost assets");

    // The three that straddle the boundary, by id.
    const auto keyAt = [&](u32 slot) {
        for (const auto& [k, v] : cooked)
            if (v == slot) return k;
        return std::string();
    };
    for (u32 slot : {49u, 50u, 51u}) {
        const std::string k = keyAt(slot);
        if (!Check(!k.empty(), "no asset ended up at slot " + std::to_string(slot))) continue;
        CheckEq(uap::PackIndexOf(slot), slot / 50u, "PackIndexOf disagrees with slot/50");
        // The real test: is it physically in that pack file?
        uap::PackReader reader;
        const fs::path packFile =
            s.packDir / ("Test_" + std::to_string(uap::PackIndexOf(slot)) + ".uap");
        Check(reader.Open(packFile) && reader.Contains(k),
              "slot " + std::to_string(slot) + " ('" + k + "') is not in pack " +
                  std::to_string(uap::PackIndexOf(slot)));
        // ...and NOT in the other one.
        uap::PackReader other;
        const u32 otherIdx = uap::PackIndexOf(slot) == 0 ? 1u : 0u;
        Check(other.Open(s.packDir / ("Test_" + std::to_string(otherIdx) + ".uap")) &&
                  !other.Contains(k),
              "slot " + std::to_string(slot) + " ('" + k + "') is ALSO in the other pack");
    }

    // Deleting the two high assets drops the pack count back to one: the pack
    // count follows the highest PACKED slot, not the asset count.
    DeleteAsset(s, keyAt(50));
    DeleteAsset(s, keyAt(51));
    u32 after = 0;
    Cook(s, s.packDir, &after);
    CheckEq(after, 1, "removing the assets past the boundary should leave one pack");

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

// 6: a cook is a pure function of the files on disk.
void TestCookDeterminism() {
    const Scratch s = MakeScratch("hbe_slotids_cook");
    for (const char* k : {"Zed/z.uaf", "Alpha/a.uaf", "Mid/m.hbmat", "Alpha/b.hbmat"})
        CreateAsset(s, k);

    const fs::path outA = s.root / "PackA";
    const fs::path outB = s.root / "PackB";
    u32 packsA = 0, packsB = 0;
    const std::map<std::string, u32> a = Cook(s, outA, &packsA);
    const std::map<std::string, u32> b = Cook(s, outB, &packsB);
    CheckEq(packsB, packsA, "two cooks in a row disagreed about the pack count");
    Check(a == b, "two cooks in a row produced different slot layouts");
    Check(!a.empty(), "the cook produced nothing");
    Check(ReadBytes(outA / "Test_0.uap") == ReadBytes(outB / "Test_0.uap"),
          "two cooks in a row produced different pack BYTES (a patch would re-download "
          "an unchanged pack)");

    // Adding an asset whose path sorts FIRST must not move anything. This is the
    // exact failure the removed `compact` option caused on every shipped build.
    CreateAsset(s, "Aaaa/inserted.uaf");
    const fs::path outC = s.root / "PackC";
    const std::map<std::string, u32> c = Cook(s, outC);
    for (const auto& [key, slot] : a) {
        CheckEq(c.count(key) ? c.at(key) : kUnassigned, slot,
                "'" + key + "' moved when an early-sorting asset was added");
    }
    Check(c.count("Aaaa/inserted.uaf") != 0, "the inserted asset was not packed");
    Check(ReadBytes(outA / "Test_0.uap") != ReadBytes(outC / "Test_0.uap"),
          "adding an asset did not change the pack it went into (the test is not testing "
          "anything)");

    // An asset that this cook does NOT pack still holds its id: toggling
    // BuildSettings::onlyReferenced must not renumber the assets that do ship.
    {
        std::set<std::string> filter;
        for (const auto& [key, slot] : c)
            if (key != "Zed/z.uaf") filter.insert(key);
        uap::WriteOptions wo;
        wo.compress = false;
        wo.filter = &filter;
        const fs::path outD = s.root / "PackD";
        std::error_code ec;
        fs::create_directories(outD, ec);
        Check(uap::WritePacks(outD, "Test", s.assets, s.manifest, wo).has_value(),
              "the filtered cook failed");
        uap::PackSet set;
        Check(set.Open(outD, "Test"), "the filtered pack set would not open");
        for (const uap::Entry& e : set.Entries()) {
            CheckEq(e.slot, c.count(e.path) ? c.at(e.path) : kUnassigned,
                    "'" + e.path + "' was renumbered by a FILTERED cook");
        }
        Check(!set.Contains("Zed/z.uaf"), "the filter did not filter");
        // ...and the excluded asset still owns its number afterwards.
        CheckEq(OnDisk(s, "Zed/z.uaf"), c.at("Zed/z.uaf"),
                "an asset excluded by the filter lost its id");
    }

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

// 5: the migration - deterministic, seeded, and a no-op on a second run.
void TestMigration() {
    const Scratch s = MakeScratch("hbe_slotids_migrate");
    // A project as it exists BEFORE the feature: assets with no ids at all.
    const std::vector<std::string> keys = {"Zed/z.uaf",   "Alpha/a.uaf", "Mid/m.hbmat",
                                           "Alpha/b.uaf", "Bin/bake.hbpaint"};
    std::error_code ec;
    for (const std::string& k : keys) {
        const fs::path p = s.assets / fs::path(k);
        const std::string ext = assets::NormalizeExtension(p);
        if (ext == ".uaf") WriteTex(p);
        else if (ext == ".hbmat") WriteMat(p);
        else { // a binary bake: packable, but it cannot carry a field
            fs::create_directories(p.parent_path(), ec);
            std::ofstream out(p, std::ios::binary);
            const u32 magic = 0x544E5048u, ver = 1;
            out.write(reinterpret_cast<const char*>(&magic), 4);
            out.write(reinterpret_cast<const char*>(&ver), 4);
        }
    }
    Check(!CanEmbedSlot(".hbpaint"), "a binary bake must not claim it can embed an id");
    Check(CanEmbedSlot(".uaf") && CanEmbedSlot(".hbmat") && CanEmbedSlot(".hbscene"),
          "a .uaf / JSON asset must be able to embed an id");

    // A seed manifest standing in for the project's `.ship.uapmanifest`: it pins
    // two assets to the ids they already ship at, so the first cook after the
    // migration moves as few packs as possible.
    const fs::path seed = s.root / "Test.ship.uapmanifest";
    Check(SaveRememberedSlots(seed, {{"Zed/z.uaf", 7}, {"Alpha/a.uaf", 3}}),
          "could not write the seed manifest");

    // DRY RUN first: it must report the whole plan and write NOTHING.
    const MigrateStats dry = MigrateSlotIds(s.assets, s.manifest, seed, /*dryRun*/ true);
    CheckEq(dry.scanned, 5, "the dry run did not scan every packable asset");
    CheckEq(dry.already, 0, "nothing should have had an id yet");
    CheckEq(dry.seeded, 2, "both seeded ids should have been honoured");
    CheckEq(dry.failed, 0, "the dry run reported failures");
    CheckEq(dry.cannotEmbed, 1, "the binary bake should be counted as manifest-only");
    CheckEq(static_cast<u32>(dry.plan.size()), 5, "the plan does not cover every asset");
    for (const std::string& k : keys)
        CheckEq(OnDisk(s, k), kUnassigned, "the DRY RUN wrote an id into '" + k + "'");
    Check(!fs::exists(s.manifest, ec), "the DRY RUN wrote the manifest");

    // APPLY.
    const MigrateStats run = MigrateSlotIds(s.assets, s.manifest, seed, /*dryRun*/ false);
    CheckEq(run.failed, 0, "the migration reported failures");
    CheckEq(run.stamped, 4, "the migration should have stamped the four embeddable assets");
    CheckEq(OnDisk(s, "Zed/z.uaf"), 7, "the seeded id was not honoured");
    CheckEq(OnDisk(s, "Alpha/a.uaf"), 3, "the seeded id was not honoured");
    // Everything else takes the lowest free id in sorted-path order:
    // Alpha/b.uaf -> 0, Bin/bake.hbpaint -> 1 (manifest only), Mid/m.hbmat -> 2.
    CheckEq(OnDisk(s, "Alpha/b.uaf"), 0, "unseeded assets are not assigned lowest-free in "
                                         "sorted-path order");
    CheckEq(OnDisk(s, "Mid/m.hbmat"), 2, "unseeded assets are not assigned lowest-free in "
                                         "sorted-path order");
    {
        const std::map<std::string, u32> mem = LoadRememberedSlots(s.manifest);
        Check(mem.count("Bin/bake.hbpaint") != 0 && mem.at("Bin/bake.hbpaint") == 1,
              "the binary bake's id was not remembered in the manifest");
    }

    // RE-RUN: a no-op. Same numbers, nothing stamped, nothing failed.
    const MigrateStats again = MigrateSlotIds(s.assets, s.manifest, seed, /*dryRun*/ false);
    CheckEq(again.scanned, 5, "the re-run did not scan every packable asset");
    CheckEq(again.already, 4, "the re-run did not see the ids it just wrote");
    CheckEq(again.stamped, 0, "the re-run stamped something - it is not idempotent");
    CheckEq(again.failed, 0, "the re-run reported failures");
    CheckEq(OnDisk(s, "Zed/z.uaf"), 7, "the re-run moved a seeded id");
    CheckEq(OnDisk(s, "Alpha/b.uaf"), 0, "the re-run moved an assigned id");
    CheckEq(OnDisk(s, "Mid/m.hbmat"), 2, "the re-run moved an assigned id");
    Check(LoadRememberedSlots(s.manifest).at("Bin/bake.hbpaint") == 1,
          "the re-run moved the binary bake's manifest id");
    // And a third run, with no seed at all, still changes nothing - the ids are
    // in the files now, so the seed has stopped mattering.
    const MigrateStats third = MigrateSlotIds(s.assets, s.manifest, {}, /*dryRun*/ false);
    CheckEq(third.stamped, 0, "a seedless re-run stamped something");
    CheckEq(OnDisk(s, "Zed/z.uaf"), 7, "a seedless re-run moved an id");

    fs::remove_all(s.root, ec);
}

// A collision (two assets copied in from different projects with the same id)
// must be resolved deterministically and loudly - never by silently dropping one
// out of the pack, which the manifest writer's old map-inversion did.
void TestCollision() {
    const Scratch s = MakeScratch("hbe_slotids_collide");
    WriteTex(s.assets / "A/dup.uaf");
    WriteTex(s.assets / "B/dup.uaf");
    Check(WriteSlot(s.assets / "A/dup.uaf", 4), "could not stamp the first duplicate");
    Check(WriteSlot(s.assets / "B/dup.uaf", 4), "could not stamp the second duplicate");

    const std::map<std::string, u32> cooked = Cook(s, s.packDir);
    Check(cooked.size() == 2, "a slot collision dropped an asset from the packs");
    Check(cooked.count("A/dup.uaf") && cooked.at("A/dup.uaf") == 4,
          "the lexicographically LOWER path should keep the contested id");
    Check(cooked.count("B/dup.uaf") && cooked.at("B/dup.uaf") != 4,
          "the loser of a collision kept the contested id");

    // The repair is deterministic: --migrate-slots re-numbers the loser in place,
    // and a re-run is then a no-op.
    const MigrateStats fix = MigrateSlotIds(s.assets, s.manifest, {}, /*dryRun*/ false);
    Check(fix.collisions >= 1, "the migration did not report the collision");
    Check(OnDisk(s, "A/dup.uaf") != OnDisk(s, "B/dup.uaf"),
          "the migration left two assets sharing an id");
    CheckEq(MigrateSlotIds(s.assets, s.manifest, {}, false).stamped, 0,
            "the repair is not idempotent");

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

// A BINARY BAKE that is renamed or moved. `.hbpaint` / `.hbgi` / `.hbprobe` /
// `.hbfrac` can carry no embedded id (their producers rewrite the whole file on
// every bake), so the manifest - keyed by pack PATH - is their only authority.
// Without slots::RekeyAsset a move is indistinguishable from delete + create, and
// dragging a Paint/ folder into a subfolder renumbers every canvas in it and
// rewrites every pack they touch. The editor's rename and drag-to-folder paths
// both call it; this proves the mechanism it calls.
void TestBakeMove() {
    const Scratch s = MakeScratch("hbe_slotids_bakemove");
    std::error_code ec;
    const auto writeBake = [&](const char* key) {
        const fs::path p = s.assets / fs::path(key);
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        const u32 magic = 0x544E5048u, ver = 1;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&ver), 4);
    };
    writeBake("Paint/one.hbpaint");
    writeBake("Paint/sub/two.hbpaint");
    writeBake("Other/keep.hbpaint");
    const u32 one = StampAsset(s.assets, s.manifest, s.assets / "Paint/one.hbpaint");
    const u32 two = StampAsset(s.assets, s.manifest, s.assets / "Paint/sub/two.hbpaint");
    const u32 keep = StampAsset(s.assets, s.manifest, s.assets / "Other/keep.hbpaint");
    Check(one != two && two != keep, "the three bakes did not get distinct ids");

    // A single FILE rename.
    fs::rename(s.assets / "Other/keep.hbpaint", s.assets / "Other/renamed.hbpaint", ec);
    CheckEq(RekeyAsset(s.assets, s.manifest, s.assets / "Other/keep.hbpaint",
                       s.assets / "Other/renamed.hbpaint"),
            1, "renaming a bake did not re-key exactly one manifest entry");

    // A whole FOLDER move, which is what the asset browser's drag-into-folder does.
    fs::create_directories(s.assets / "Archive", ec);
    fs::rename(s.assets / "Paint", s.assets / "Archive/Paint", ec);
    CheckEq(RekeyAsset(s.assets, s.manifest, s.assets / "Paint", s.assets / "Archive/Paint"), 2,
            "moving a FOLDER of bakes did not re-key every entry beneath it");

    const std::map<std::string, u32> mem = LoadRememberedSlots(s.manifest);
    Check(mem.count("Paint/one.hbpaint") == 0 && mem.count("Other/keep.hbpaint") == 0,
          "the manifest still remembers a bake at its OLD path");
    CheckEq(mem.count("Archive/Paint/one.hbpaint") ? mem.at("Archive/Paint/one.hbpaint")
                                                   : kUnassigned,
            one, "a moved bake did not carry its id");
    CheckEq(mem.count("Archive/Paint/sub/two.hbpaint") ? mem.at("Archive/Paint/sub/two.hbpaint")
                                                       : kUnassigned,
            two, "a bake nested under a moved folder did not carry its id");
    CheckEq(mem.count("Other/renamed.hbpaint") ? mem.at("Other/renamed.hbpaint") : kUnassigned,
            keep, "a renamed bake did not carry its id");

    // And the cook agrees - which is the property that actually matters, because
    // it is what decides whether those packs change for every player.
    const std::map<std::string, u32> cooked = Cook(s, s.packDir);
    CheckEq(cooked.count("Archive/Paint/one.hbpaint") ? cooked.at("Archive/Paint/one.hbpaint")
                                                      : kUnassigned,
            one, "the cook renumbered a moved bake");
    CheckEq(cooked.count("Other/renamed.hbpaint") ? cooked.at("Other/renamed.hbpaint")
                                                  : kUnassigned,
            keep, "the cook renumbered a renamed bake");

    fs::remove_all(s.root, ec);
}

// EXTRA FILES share the slot space and can hold no id. The 100-odd compiled
// shaders and `__project.hbproj` exist ONLY as manifest rows, so anything that
// hands out numbers has to reserve them from every map it is given - including a
// SEED manifest, which is where they all lived when the project had two ledgers.
// Missing that, the first asset created after a migration lands on a shader's
// number and relocates it, permanently and silently.
void TestExtraFileReservation() {
    const Scratch s = MakeScratch("hbe_slotids_extras");
    std::error_code ec;
    WriteMat(s.assets / "AAA/first.hbmat"); // sorts first; would take id 0
    WriteTex(s.assets / "AAA/second.uaf");

    Check(!IsAssetSlotKey("Shaders/ApplyHalfRes.ps.dxil") && !IsAssetSlotKey("__project.hbproj"),
          "an ExtraFile key was misclassified as an asset");
    Check(IsAssetSlotKey("AAA/first.hbmat"), "an asset key was misclassified as an ExtraFile");

    // The seed holds the extras (and nothing else) - exactly the shape of a
    // shipping manifest before the assets were ever stamped.
    const fs::path seed = s.root / "Test.ship.uapmanifest";
    Check(SaveRememberedSlots(seed, {{"Shaders/ApplyHalfRes.ps.dxil", 0},
                                     {"__project.hbproj", 1}}),
          "could not write the seed manifest");

    const MigrateStats run = MigrateSlotIds(s.assets, s.manifest, seed, /*dryRun*/ false);
    CheckEq(run.failed, 0, "the migration reported failures");
    CheckEq(OnDisk(s, "AAA/first.hbmat"), 2,
            "an asset was given an id reserved by a compiled shader in the SEED manifest");
    CheckEq(OnDisk(s, "AAA/second.uaf"), 3, "the second asset did not continue past the extras");

    // ...and the reservations are PERSISTED, so the editor's allocator (which only
    // ever sees the target manifest) inherits them instead of rediscovering the
    // same hole on the next import.
    const std::map<std::string, u32> mem = LoadRememberedSlots(s.manifest);
    CheckEq(mem.count("Shaders/ApplyHalfRes.ps.dxil") ? mem.at("Shaders/ApplyHalfRes.ps.dxil")
                                                      : kUnassigned,
            0, "the shader reservation was not written into the target manifest");
    CheckEq(mem.count("__project.hbproj") ? mem.at("__project.hbproj") : kUnassigned, 1,
            "the .hbproj reservation was not written into the target manifest");

    // The next CREATED asset must also respect them.
    CheckEq(CreateAsset(s, "AAA/third.hbmat"), 4,
            "an asset created after the migration took a reserved ExtraFile id");

    // A cook that packs NO extras (the dev --pack) must not erase the reservations
    // from the shared ledger on its way past - that is what would let the next cook
    // renumber every shader.
    Cook(s, s.packDir);
    const std::map<std::string, u32> after = LoadRememberedSlots(s.manifest);
    CheckEq(after.count("Shaders/ApplyHalfRes.ps.dxil") ? after.at("Shaders/ApplyHalfRes.ps.dxil")
                                                        : kUnassigned,
            0, "a cook with no extras ERASED the shader reservations from the ledger");
    CheckEq(after.count("__project.hbproj") ? after.at("__project.hbproj") : kUnassigned, 1,
            "a cook with no extras erased the .hbproj reservation");

    fs::remove_all(s.root, ec);
}

// The slot contract as the project owner wrote it, encoded verbatim so the spec is
// executable rather than prose. Every other test here covers a mechanism; this one
// covers the AGREEMENT, and is deliberately written in their names and their order:
//
//   Tree=0, Rock=1, Player=2, Ghost=3
//   Remove Rock  -> slot 1 empty
//   Import Chair -> Chair takes slot 1
//   Remove Ghost -> slot 3 empty
//   Import Table -> Table takes slot 3
//   Tree, Player and every other existing asset stay where they were.
//
// It intentionally overlaps TestAllocationLifecycle. If a future change breaks the
// contract, this is the failure that names WHAT was promised rather than which
// internal invariant tripped.
void TestOwnerSpecExample() {
    const Scratch s = MakeScratch("hbe_slotids_spec");

    CheckEq(CreateAsset(s, "World/Tree.uaf"), 0, "Tree should take slot 0");
    CheckEq(CreateAsset(s, "World/Rock.uaf"), 1, "Rock should take slot 1");
    CheckEq(CreateAsset(s, "World/Player.uaf"), 2, "Player should take slot 2");
    CheckEq(CreateAsset(s, "World/Ghost.uaf"), 3, "Ghost should take slot 3");

    DeleteAsset(s, "World/Rock.uaf"); // slot 1 becomes EMPTY - not reclaimed by shifting
    CheckEq(CreateAsset(s, "World/Chair.uaf"), 1,
            "Chair must fill the LOWEST empty slot (1, freed by Rock)");

    DeleteAsset(s, "World/Ghost.uaf"); // slot 3 becomes EMPTY
    CheckEq(CreateAsset(s, "World/Table.uaf"), 3,
            "Table must fill the LOWEST empty slot (3, freed by Ghost)");

    // The whole point: nothing that survived moved, on disk, at any step.
    CheckEq(OnDisk(s, "World/Tree.uaf"), 0, "Tree MOVED - existing assets must never shift");
    CheckEq(OnDisk(s, "World/Player.uaf"), 2, "Player MOVED - existing assets must never shift");
    CheckEq(OnDisk(s, "World/Chair.uaf"), 1, "Chair did not keep the slot it was given");
    CheckEq(OnDisk(s, "World/Table.uaf"), 3, "Table did not keep the slot it was given");

    // And the next import goes past the high-water mark, never onto a live slot.
    CheckEq(CreateAsset(s, "World/Lamp.uaf"), 4,
            "with no holes left, the next import must extend rather than displace");

    std::error_code ec;
    fs::remove_all(s.root, ec);
}

} // namespace

bool SlotIdSelfTest() {
    g_failures = 0;
    TestOwnerSpecExample();
    TestAllocationLifecycle();
    TestPreservation();
    TestPackBoundary();
    TestCookDeterminism();
    TestMigration();
    TestCollision();
    TestBakeMove();
    TestExtraFileReservation();
    if (g_failures != 0) {
        HBE_ERROR("slotids: {} failure(s).", g_failures);
        return false;
    }
    HBE_INFO("slotids: allocation, reuse, stability, slot/{} pack boundary, migration "
             "idempotence and cook determinism all hold.",
             uap::kSlotsPerPack);
    return true;
}

} // namespace hbe::slots
