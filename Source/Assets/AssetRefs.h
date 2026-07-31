// Assets/AssetRefs.h - the DEPENDENCY CLOSURE over the asset graph.
//
// "Pack only referenced assets" (BuildSettings::onlyReferenced) is only safe if
// the reference walk is TOTAL. It was not: the old walk followed 8 fields of a
// `.hbscene` one hop deep and nothing else, so a `.hbschem`, a `.hbprefab`, a
// `.hbchar`, a `.hbmusic`'s stems, a `.hbcutscene`'s dialogue and its voicelines
// all packed out. Nothing crashes when they do - a missing schematic is swallowed
// by SchematicSystem, a missing UI clip is negative-cached without a log - so the
// build boots at full frame rate and quietly does nothing. That failure is
// invisible at cook time and unattributable at play time, which is why this file
// exists.
//
// THE DESIGN, in three parts:
//
//   1. GENERIC, NOT DECLARED. A JSON asset is walked by scanning EVERY string
//      value in the document (see assets::RefScan::JsonScan). A declared field
//      list is the same hand-written walk with a new address: it restates schema
//      that already lives in the parser, and it drifts silently the moment a
//      field is added. The value scan needs no field list, so a new field - or a
//      whole new schematic node type carrying a path literal - is covered with
//      ZERO edits. Formats with no JSON to scan declare a Hook; formats that
//      provably reference nothing declare Leaf. The registry
//      (Assets/AssetFormats.h) forces every runtime-loaded row to pick one, and
//      --test-assetformats fails a row that did not.
//
//   2. A WORKLIST TO FIXPOINT, not a flat pass. Anything discovered is re-opened
//      and scanned in turn, so a texture named by a `.hbmat` named by a
//      `.hbprefab` named by a `.hbscene` is reached.
//
//   3. NORMALISATION IS PART OF THE CONTRACT. uap::WritePacks matches its filter
//      by EXACT STRING against the Assets-relative pack key, but references are
//      not guaranteed to be authored that way - vfs::ReadByFilename exists
//      precisely because some carry a bare filename. A bare `"wood.uaf"` against
//      a pack key of `"Textures/wood.uaf"` misses the filter, drops the asset,
//      and the runtime's own filename fallback then cannot save it (it is not in
//      the pack). So every reference is resolved to a real file and returned as
//      the canonical key. An AMBIGUOUS bare filename (two directories, one name)
//      is reported unresolved rather than guessed - the runtime's fallback would
//      be equally arbitrary, so guessing here would only hide the ambiguity.
//
// FALSE POSITIVES ARE ACCEPTED, DELIBERATELY. A string that both looks like a
// packable path and names a real file is included even if the field it sits in
// has no runtime consumer (`.hbchar::weldCache`, `.hbfrac::interiorMaterial`,
// `.hbevent`). That over-packs by a handful of files. The alternative - an
// exclusion list - is a hand-maintained thing that goes stale, i.e. exactly the
// bug this file removes. Over-packing costs bytes; under-packing ships broken.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::assets {

// --- Reference normalisation ------------------------------------------------

enum class RefStatus : u8 {
    Resolved,  // `key` is a real file under Assets/
    NotFound,  // nothing on disk matches, by path or by filename
    Ambiguous, // a bare filename that matches two or more files
    // A file exists whose path differs from the reference ONLY in letter case.
    // This is NOT resolvable, and treating it as resolved is worse than not
    // finding it at all: NTFS is case-insensitive, so the editor reads the file
    // happily off disk and everything looks correct, while the cook packs it
    // under its REAL name and the shipped runtime looks it up under the AUTHORED
    // one. uap::PackSet::Read and vfs::ReadByFilename both compare bytes
    // (Assets/UAP.cpp, Assets/VFS.cpp), so the lookup misses, there is no loose
    // file left to fall back to, and the asset is silently absent in the shipped
    // build ONLY. `key` carries the real on-disk path so the message can name the
    // one-character fix.
    CaseMismatch,
};

struct ResolvedRef {
    RefStatus status = RefStatus::NotFound;
    // Assets-relative, forward slashes. Valid iff Resolved; on CaseMismatch it
    // holds the actual on-disk path, for the diagnostic only.
    std::string key;
};

// Turns authored reference strings into pack keys. Builds a one-shot index of
// every packable file under `assetsDir` (path and filename), so resolution is a
// hash lookup rather than the recursive rescan vfs::ReadByFilename does per miss.
class ReferenceResolver {
public:
    explicit ReferenceResolver(const std::filesystem::path& assetsDir);

    ResolvedRef Resolve(const std::string& raw) const;

    // Gate 1 of the scan: does `raw`, once stripped of the `uaf:` prefix and a
    // trailing `#<submesh>`, even LOOK like a packable asset path? This is what
    // makes scanning every string in a document cheap and quiet - entity names,
    // tags, guids, colour hex, enum strings, node titles, objective text and flag
    // names carry no packable extension, so they never reach the filesystem.
    // Writes the cleaned Assets-relative form to `outRel` when it passes.
    static bool LooksLikeAssetRef(const std::string& raw, std::string* outRel = nullptr);

    usize IndexedFileCount() const { return byRel_.size(); }

private:
    std::filesystem::path assetsDir_;
    // EXACT-CASE first, lower-cased second. The verdict must be byte-exact because
    // every runtime lookup is (see RefStatus::CaseMismatch); the lower-cased maps
    // exist only so a near-miss can be DIAGNOSED instead of reported as "not
    // found", which on a case-insensitive filesystem reads as a lie.
    std::set<std::string> exactRel_;                                   // rel, byte-exact
    std::unordered_map<std::string, std::string> byRel_;               // lower(rel) -> rel
    std::unordered_map<std::string, std::vector<std::string>> byName_; // lower(name) -> rels
    std::unordered_map<std::string, std::vector<std::string>> byExactName_; // name -> rels
};

// --- The closure ------------------------------------------------------------

// A reference that names no file. The game is ALREADY broken when this happens
// (the file does not exist anywhere); the only question is whether a human hears
// about it at cook time or never.
struct MissingRef {
    std::string from;   // pack key of the file that names it ("<project>" for a root)
    std::string raw;    // the string exactly as authored
    std::string reason; // "not found" | "ambiguous filename"
};

struct ClosureResult {
    // False = the closure could not be PROVEN total, so a filtered cook would be
    // guesswork. Set when any reference is unresolvable, any reachable file
    // cannot be read/parsed, or any reached extension has no RefScan.
    bool ok = false;
    std::set<std::string> included;      // pack keys - this is the pack filter
    std::vector<MissingRef> missing;
    std::vector<std::string> unreadable; // reachable files that would not parse
    std::vector<std::string> excluded;   // packable files on disk NOT reached
    std::map<std::string, u32> includedByExt;
    std::map<std::string, u32> excludedByExt;
    u32 rootCount = 0;
};

struct ClosureOptions {
    // Roots that live outside the asset graph - the project's startupScene,
    // bootDocument, every uiDocuments[] entry, musicGraph, and the input-icon
    // glyph textures. Accepted in whatever form the project file holds them
    // (Assets-relative or bare); each is normalised like any other reference.
    std::vector<std::string> roots;
    // Also treat EVERY .hbscene / .hbui / .hbprefab under Assets/ as a root.
    // These are entry points selected by paths the scan cannot see (a level name
    // in a .hbsave, a dev-menu jump, a spawner picked at runtime), and they are
    // small JSON - sweeping them is cheap insurance. Everything else becomes
    // reachable ONLY through the closure, which is the point.
    bool sweepEntryPoints = true;
};

ClosureResult ComputeClosure(const std::filesystem::path& assetsDir,
                             const ClosureOptions& options);

// The per-file reference extraction the closure runs, exposed so a verifier can
// re-walk one asset on its own (--test-packclosure re-walks every entry of a
// COOKED pack set and asserts each reference resolves inside it). Returns false
// when the file could not be read or its extension has no RefScan.
bool CollectFileRefs(const std::filesystem::path& file, std::vector<std::string>& out);

// The cook-time report: what was INCLUDED and what was EXCLUDED, with counts and
// a per-extension breakdown, plus every unresolvable reference. A silent cook is
// what made the old hole invisible, so this always logs, even on success.
void LogClosureReport(const ClosureResult& result, bool onlyReferenced,
                      bool allowMissingRefs);

// --test-packclosure: builds a synthetic project exercising every format in the
// reference matrix (each referencing the next), and proves the closure reaches
// all of them, reaches a purely transitive leaf, excludes orphans only when the
// filter is on, and reports an unresolvable reference instead of packing around
// it. Headless; no GPU, no window, no project.
bool PackClosureSelfTest();

} // namespace hbe::assets
