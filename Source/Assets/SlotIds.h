// Assets/SlotIds.h - the asset's PACK SLOT, stored with the asset.
//
// Every packable asset owns a small dense integer - its pack slot. Slot / 50
// selects the pack file it lands in (uap::kSlotsPerPack), slot % 50 its position
// inside that pack. The number is assigned ONCE, when the asset is created, and
// it never moves for as long as the asset lives; deleting an asset frees its
// number, and the next asset created takes the lowest free one.
//
// WHY THE NUMBER LIVES IN THE ASSET, not in a sidecar:
//   The cooker used to derive every slot from a `.uapmanifest` keyed by
//   Assets-relative PATH, and the shipping cook set `compact`, which threw that
//   manifest away and re-derived dense slots 0..N-1 in sorted-path order. So
//   adding one asset whose path sorted early shifted every asset after it and
//   rewrote every pack file. Shipped pack stability was exactly zero. Path-keying
//   also makes a rename indistinguishable from delete + create.
//   With the id inside the file, moving or renaming an asset carries its number
//   along, and a cook is a pure function of the files on disk.
//
// THE AUTHORITY ORDER, and there is only one authority per file:
//   1. The asset file's own embedded id. Always wins.
//   2. The manifest's remembered id for that exact pack key. Used ONLY when (1)
//      is absent - for the 107 compiled shaders and the `.hbproj` that ship as
//      uap::ExtraFile and are structurally incapable of carrying a field, for the
//      binary bake formats listed under CanEmbedSlot below, and for an asset
//      whose saver rebuilt its JSON from scratch and dropped the key.
//   3. Lowest free. Only when neither exists.
//
// WHAT CARRIES AN ID, and what does not - see CanEmbedSlot().
//
// NOTHING HERE WRITES TO A PROJECT ON ITS OWN. Stamping happens at exactly two
// kinds of moment: an asset is created (importer::Import and the editor's
// create-new paths call StampNewAssets / StampAsset), or the operator runs
// --migrate-slots --apply. A cook never writes into Assets/.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace hbe::slots {

// "this file has no id yet". Distinct from slot 0, which is a real slot.
inline constexpr u32 kUnassigned = 0xFFFFFFFFu;

// True when this extension can hold an id INSIDE the file (authority tier 1).
//
// Yes:  `.uaf` (a flagged header field - see UAF.cpp) and every JSON engine
//       asset (a top-level "packSlot" key).
// No:   `.hbpaint` / `.hbgi` / `.hbprobe` / `.hbfrac` - binary bakes whose
//       producers rewrite the whole file on every bake, so an embedded field
//       would be destroyed as fast as it was written. They use tier 2, exactly
//       like the compiled shaders do, and are keyed by pack path.
bool CanEmbedSlot(const std::string& ext);

// The id embedded in `file`. kUnassigned when the format can hold one but does
// not yet; nullopt when the file cannot be read or is not valid for its format
// (which a caller must NOT confuse with "has no id" - a corrupt file must not
// silently take a fresh slot).
std::optional<u32> ReadSlot(const std::filesystem::path& file);

// Stamps `slot` into `file`, preserving everything else about it.
//   `.uaf` - patches the header in place when the file already carries the
//            field, otherwise rewrites it once (streamed through a temp file and
//            renamed, so an interrupted stamp cannot leave a truncated asset).
//   JSON   - re-emits the document with a top-level "packSlot".
// Returns false when the format cannot embed one, or on any IO/parse failure.
bool WriteSlot(const std::filesystem::path& file, u32 slot);

// --- The allocator ----------------------------------------------------------
//
// Deterministic and purely in-memory: it decides numbers, it never touches a
// file. Both the cooker and the stamper drive it, which is what makes a cook and
// an import agree about who owns what.
class SlotTable {
public:
    void Clear();

    // Records `key` at `slot`. False (and nothing recorded) when the slot is
    // already taken - the caller then falls back to ClaimLowestFree.
    bool Claim(const std::string& key, u32 slot);
    // Assigns the lowest free slot to `key` and returns it.
    u32 ClaimLowestFree(const std::string& key);
    // Frees `key`'s slot (the next ClaimLowestFree can hand that number out).
    void Release(const std::string& key);

    u32 SlotOf(const std::string& key) const;
    bool InUse(u32 slot) const;
    u32 LowestFree() const;
    usize Size() const { return byKey_.size(); }
    const std::map<std::string, u32>& All() const { return byKey_; }

private:
    std::map<std::string, u32> byKey_;
    std::set<u32> used_;
    u32 nextFreeHint_ = 0;
};

// --- The persisted memory (authority tier 2) --------------------------------

// Reads the `slots` block of a `.uapmanifest` (empty when missing/corrupt).
std::map<std::string, u32> LoadRememberedSlots(const std::filesystem::path& manifestPath);
// Writes it back, sorted by slot so a diff of the manifest stays readable.
bool SaveRememberedSlots(const std::filesystem::path& manifestPath,
                         const std::map<std::string, u32>& slots);

// True when `key` names a packable asset under Assets/. False for a
// uap::ExtraFile - the compiled shaders and `__project.hbproj`, which share the
// slot space but live outside Assets/ and can never be checked for existence.
bool IsAssetSlotKey(const std::string& key);

// Folds an allocator's decisions into a persisted slot map, IN PLACE:
//   - an ASSET entry whose file no longer exists loses its entry (that, and only
//     that, is what frees a deleted asset's number for the next import);
//   - a NON-ASSET entry is kept exactly as it is, forever - nothing here can tell
//     whether a shader still exists, and handing its number to an asset would
//     collide at the next cook;
//   - everything `table` currently holds is recorded.
// The cooker uses this rather than writing `table.All()` wholesale, because a cook
// that packs no extras (the dev `--pack`) would otherwise erase every shader
// reservation from the shared ledger and let the next cook renumber them.
void MergeRememberedSlots(const std::filesystem::path& assetsDir, const SlotTable& table,
                          std::map<std::string, u32>& remembered);

// --- Stamping (the only paths that write into a project) --------------------

// Gives `file` an id and writes it in, unless it already has one. Prefers the id
// the manifest remembers for that pack key, so re-importing an asset to fix its
// gamma keeps its number instead of reshuffling the packs. Returns the id in
// effect (kUnassigned when the format cannot embed one - the manifest carries it
// instead, and this still records it there).
u32 StampAsset(const std::filesystem::path& assetsDir,
               const std::filesystem::path& manifestPath,
               const std::filesystem::path& file);

// Same, for a whole operation: every packable file under `assetsDir` last
// written at or after `since` that has no id gets one. This is what covers an
// import's BY-PRODUCTS - one model import writes a `.uaf` per texture and a
// `.hbmat` per material, none of which the caller sees. Returns how many were
// stamped. Files older than `since` are never touched.
u32 StampNewAssets(const std::filesystem::path& assetsDir,
                   const std::filesystem::path& manifestPath,
                   std::filesystem::file_time_type since);

// A convenient `since` for "right now, with slack": file timestamps and the
// steady clock do not tick together, so a file written microseconds ago can
// carry a stamp fractionally in the past.
std::filesystem::file_time_type MarkNow();

// Carries slot memory across a RENAME or a MOVE that has already happened on disk.
// `from`/`to` are absolute paths and may name a FILE or a DIRECTORY (the editor's
// drag-a-tile-into-a-folder moves whole folders); a directory re-keys every
// remembered entry beneath it.
//
// Needed because the manifest is keyed by pack PATH, and for the binary bake
// formats (.hbpaint, .hbgi, .hbprobe, .hbfrac - see CanEmbedSlot) the manifest is
// the ONLY authority: without this, dragging a Paint/ folder into a subfolder is
// indistinguishable from deleting 44 assets and creating 44 new ones, which
// renumbers all of them and rewrites every pack they touch. It also covers the
// window in which an embeddable asset has lost its embedded id (a saver that
// rebuilt its JSON) and is temporarily relying on tier 2.
//
// Returns the number of entries re-keyed. Writes the manifest only if that is
// non-zero. Never touches the asset files.
u32 RekeyAsset(const std::filesystem::path& assetsDir,
               const std::filesystem::path& manifestPath,
               const std::filesystem::path& from, const std::filesystem::path& to);

// --- Migration (--migrate-slots) --------------------------------------------

struct MigrateStats {
    u32 scanned = 0;        // packable files seen
    u32 already = 0;        // already carried an id
    u32 stamped = 0;        // would be / were given one
    u32 seeded = 0;         // of those, took the id the seed manifest remembered
    u32 cannotEmbed = 0;    // binary bakes: id recorded in the manifest instead
    u32 collisions = 0;     // two files claiming one id (lower path kept it)
    u32 failed = 0;         // could not be read, or could not be written
    // Every id the run would assign, in slot order: (pack key, slot, seeded?).
    std::vector<std::tuple<std::string, u32, bool>> plan;
};

// Assigns an id to every packable asset in `assetsDir` that lacks one, in a
// STABLE, deterministic order, and (unless `dryRun`) writes them in.
//
// Order, chosen so the first cook after the migration is the SMALLEST possible
// patch: ids the seed manifest already remembers for a still-present path are
// kept exactly; everything else takes the lowest free id in sorted-path order.
// Re-running is therefore a no-op - every asset already carries the id this
// function would compute.
//
// `seedManifest` may be empty (then nothing is seeded). Pass the project's
// `<Name>.ship.uapmanifest` to reproduce the currently shipped layout.
MigrateStats MigrateSlotIds(const std::filesystem::path& assetsDir,
                            const std::filesystem::path& manifestPath,
                            const std::filesystem::path& seedManifest, bool dryRun);

// --test-slotids: the gate for everything above. Headless; no GPU, no window, no
// project - it builds and deletes its own scratch project.
bool SlotIdSelfTest();

} // namespace hbe::slots
