// Assets/AssetFormats.h - the ONE registry of every file format the engine knows.
//
// Two tables live here, and every consumer derives from them instead of keeping
// its own hard-coded extension list (which is how they drift apart):
//
//   ENGINE ASSETS  - files the engine itself authors and loads (`.uaf`, `.hbscene`,
//                    `.hbchar`, ...). Each entry records whether the RUNTIME loads
//                    it, which makes it a hard requirement that the pack cooker
//                    ships it: a runtime-loaded type missing from the pack simply
//                    vanishes in a shipped build, with no error until the feature
//                    silently does nothing.
//   SOURCE FORMATS - raw files the EDITOR imports into `.uaf` (`.png`, `.fbx`,
//                    `.wav`, ...), grouped by the importer branch that handles them.
//
// Consumers: Assets/UAP.cpp (IsPackableExtension), Editor/Importer.cpp
// (IsSupportedSource + the per-category dispatch), and the editor's Import file
// dialog (its filter string is GENERATED here, so it can never offer fewer
// formats than the importer accepts).
#pragma once

#include "Core/Types.h"
#include "Core/FileFilter.h" // platform::FileFilter (the import dialog's type filter)

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::assets {

// Which importer branch handles a source file (and therefore what it becomes).
enum class SourceKind : u8 {
    Image,  // -> uaf::Texture   (stb_image)
    Model,  // -> uaf::Mesh      (Assimp)
    Audio,  // -> uaf::Audio     (miniaudio decoder)
    Font,   // -> uaf::Font      (stored verbatim)
};

struct SourceFormat {
    const char* extension; // lower-case, leading dot
    SourceKind kind;
    const char* note; // human-readable, shown in editor tooling
};

// How the dependency-closure walker (Assets/AssetRefs.h) extracts a file's
// OUTBOUND asset references. Every runtimeLoaded row must declare one.
//
// `Unspecified` is the zero value ON PURPOSE: a row added without thinking
// value-initialises to the invalid state and fails --test-assetformats, instead
// of silently contributing nothing to the closure - which, with
// BuildSettings::onlyReferenced on, ships a build whose assets are quietly
// absent (no cook error, no runtime error, the feature just does nothing).
enum class RefScan : u8 {
    Unspecified = 0, // invalid; --test-assetformats fails on this
    JsonScan,        // parse as JSON; EVERY string value is a reference candidate
    Hook,            // `collect` walks the format's own (binary) payload
    Leaf,            // proven to carry no outbound references (`note` must say why)
};

// Extracts the raw, as-authored reference strings out of one file. Resolution
// (and the packable-extension test) happens in the closure, not here - a hook
// may over-report freely. Returns false when the file could not be read or is
// not this format at all, which the closure records as "reachable but unknown"
// rather than quietly treating as a leaf.
using CollectRefsFn = bool (*)(const std::filesystem::path& file,
                               std::vector<std::string>& out);

// The RefScan::Hook collectors. Declared here (rather than living beside their
// parsers) so the registry rows below can take their addresses without this
// translation unit depending on the whole asset library; defined in
// Assets/AssetRefs.cpp.
bool CollectRefsUaf(const std::filesystem::path& file, std::vector<std::string>& out);
bool CollectRefsFracture(const std::filesystem::path& file, std::vector<std::string>& out);

struct EngineAsset {
    const char* extension;  // lower-case, leading dot
    const char* name;       // display name ("Cutscene")
    bool runtimeLoaded;     // the RUNTIME reads it -> must be packable
    RefScan scan;           // how the closure walks it (see RefScan)
    CollectRefsFn collect;  // non-null IFF scan == Hook
    const char* note;
};

// The full tables (stable order; safe to iterate for UI).
const std::vector<SourceFormat>& SourceFormats();
const std::vector<EngineAsset>& EngineAssets();

// Lower-cases an extension (with its dot) for table lookups; `.PNG` -> `.png`.
std::string NormalizeExtension(const std::filesystem::path& path);

// Source-format queries (used by the importer's dispatch).
bool IsSourceFormat(const std::string& ext);
bool IsSourceKind(const std::string& ext, SourceKind kind);
// The importer branch for `ext`; only meaningful when IsSourceFormat(ext).
SourceKind KindOf(const std::string& ext);

// True when the pack cooker must include this extension. Exactly the engine-asset
// entries flagged runtimeLoaded - derived, never a second hand-written list.
bool IsPackable(const std::string& ext);

// Closure-walk classification for `ext` (Unspecified when unknown/unregistered).
RefScan RefScanOf(const std::string& ext);
// The collector for a RefScan::Hook extension (nullptr otherwise).
CollectRefsFn CollectorOf(const std::string& ext);

// --test-assetformats: the registry's own invariants, checked rather than
// assumed. Fails (logging every violation) when a runtime-loaded row leaves
// `scan` Unspecified, when a Hook row has no collector, when a non-Hook row
// carries one, when a Leaf row has no written justification in `note`, or when
// an extension is duplicated / malformed. Headless; no project needed.
bool RegistrySelfTest();

// The editor's Import-dialog type filter, built from the SOURCE FORMATS table so the
// dialog and the importer can never disagree (the old literal had silently fallen
// behind and hid .psd/.gif/.dae/.ply/.mp3/.flac). Grouped "All supported assets",
// then per-category. Extensions are bare dotless tokens; the native-dialog backend
// renders them and appends its own "All files" - this stays free of any OS filter
// format. See Core/NativeDialogs.h for how it is consumed.
std::vector<platform::FileFilter> ImportFileFilters();

} // namespace hbe::assets
