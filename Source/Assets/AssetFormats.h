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

struct EngineAsset {
    const char* extension;  // lower-case, leading dot
    const char* name;       // display name ("Cutscene")
    bool runtimeLoaded;     // the RUNTIME reads it -> must be packable
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

// Win32 OPENFILENAME filter for the editor's Import dialog, built from
// SourceFormats() so the dialog and the importer can never disagree. Returns a
// double-NUL-terminated block: "Label\0*.a;*.b\0...\0All files\0*.*\0\0".
std::wstring BuildImportDialogFilter();

} // namespace hbe::assets
