// Assets/AssetFormats.cpp
#include "Assets/AssetFormats.h"

#include <algorithm>
#include <cctype>

namespace hbe::assets {
namespace {

// --- Source formats the EDITOR imports -------------------------------------
// Images: exactly what stb_image decodes. stb has NO dds/exr/tiff/webp support,
// so those must not be listed (they would fail with a useless stb error).
// Models: exactly the Assimp importers enabled in cmake/Dependencies.cmake -
// keep the two in lockstep (see the comment there).
// Audio: exactly miniaudio's BUILT-IN decoders. Vorbis (.ogg) needs stb_vorbis
// compiled in, which this build does not do, so .ogg is deliberately absent.
const std::vector<SourceFormat> kSourceFormats = {
    // Images (stb_image)
    {".png",  SourceKind::Image, "PNG"},
    {".jpg",  SourceKind::Image, "JPEG"},
    {".jpeg", SourceKind::Image, "JPEG"},
    {".tga",  SourceKind::Image, "Targa"},
    {".bmp",  SourceKind::Image, "Windows bitmap"},
    {".psd",  SourceKind::Image, "Photoshop (flattened composite)"},
    {".gif",  SourceKind::Image, "GIF (first frame)"},
    {".hdr",  SourceKind::Image, "Radiance HDR (imported as RGBA32F linear)"},
    {".pic",  SourceKind::Image, "Softimage PIC"},
    {".ppm",  SourceKind::Image, "Portable pixmap"},
    {".pgm",  SourceKind::Image, "Portable graymap"},
    // Models (Assimp)
    {".gltf", SourceKind::Model, "glTF 2.0"},
    {".glb",  SourceKind::Model, "glTF 2.0 (binary)"},
    {".obj",  SourceKind::Model, "Wavefront OBJ"},
    {".fbx",  SourceKind::Model, "Autodesk FBX"},
    {".dae",  SourceKind::Model, "Collada"},
    {".ply",  SourceKind::Model, "Stanford PLY"},
    {".stl",  SourceKind::Model, "Stereolithography"},
    // Audio (miniaudio)
    {".wav",  SourceKind::Audio, "WAV / RIFF PCM"},
    {".mp3",  SourceKind::Audio, "MPEG layer 3"},
    {".flac", SourceKind::Audio, "FLAC"},
    // Fonts (stored verbatim; atlas baked at load)
    {".ttf",  SourceKind::Font, "TrueType"},
    {".otf",  SourceKind::Font, "OpenType"},
};

// --- Engine's own asset files ----------------------------------------------
// `runtimeLoaded` is the shipping contract: true means the RUNTIME reads this
// file, so the pack cooker MUST include it (IsPackable is derived from this
// column). A runtime-loaded type left out of the packs disappears in a shipped
// build with no error - the feature just quietly does nothing.
const std::vector<EngineAsset> kEngineAssets = {
    {".uaf",        "Asset",            true,  "meshes / textures / audio / fonts"},
    {".hbscene",    "Scene",            true,  "also the format of .hbprefab subtrees"},
    {".hbmat",      "Material",         true,  ""},
    {".hbpaint",    "Paint Canvas",     true,  "Art Editor surface paint"},
    {".hbevent",    "Audio Event",      true,  ""},
    {".hbschem",    "Schematic",        true,  "visual script"},
    {".hbdialogue", "Dialogue",         true,  "branching conversation graph"},
    {".hbcutscene", "Cutscene",         true,  ""},
    {".hbmusic",    "Music Graph",      true,  "adaptive music"},
    {".hbchar",     "Character",        true,  "modular rig; CharacterSystem loads it via the VFS"},
    {".hbprefab",   "Prefab",           true,  "SpawnSystem instantiates these at runtime"},
    {".hbuianim",   "UI Animation",     true,  "UIAnimator clip"},
    {".hbgi",       "GI Volume",        true,  "baked irradiance cache (SceneEnvironment::giSource)"},
    {".hbcharcache","Character Cache",  true,  "derived seam-welded mesh for a .hbchar"},
    {".hbfrac",     "Fracture",         true,  "pre-fractured destructible chunks + adjacency"},
    // NOT packed by extension - each for a specific reason. Do not "fix" these to
    // true: .hbproj IS read at runtime through the VFS, but it ships via the
    // separate uap::ExtraFile path (BuildShipping packs it under the virtual name
    // "__project.hbproj"), so listing it here would pack a second, wrongly-named
    // copy. .hbsave lives in the user's save directory and is written at runtime.
    {".hbproj",     "Project",          false, "packed as an ExtraFile (__project.hbproj)"},
    {".hbsave",     "Save Game",        false, "written at runtime into the user's save dir"},
    {".uapmanifest","Pack Manifest",    false, "editor-side slot bookkeeping"},
};

} // namespace

const std::vector<SourceFormat>& SourceFormats() { return kSourceFormats; }
const std::vector<EngineAsset>& EngineAssets() { return kEngineAssets; }

std::string NormalizeExtension(const std::filesystem::path& path) {
    // Accepts EITHER a full path/filename ("Scenes/Foo.hbscene") or a BARE
    // extension (".hbscene") - callers legitimately have both.
    //
    // The bare-extension case needs the explicit fallback below because
    // std::filesystem::path(".hbscene").extension() returns an EMPTY string by
    // the standard: a filename that starts with a period and contains no other
    // period is a hidden-file name (".bashrc"), not a name with an extension.
    // Re-extracting therefore silently yields "" for any caller that already
    // extracted - which is exactly what happened to UAP's IsPackableExtension,
    // and it excluded EVERY asset from the shipped packs with no error at all.
    std::string e = path.extension().string();
    if (e.empty()) {
        const std::string s = path.string();
        const bool bareExtension = s.size() > 1 && s.front() == '.' &&
                                   s.find('/') == std::string::npos &&
                                   s.find('\\') == std::string::npos;
        if (bareExtension) e = s;
    }
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool IsSourceFormat(const std::string& ext) {
    return std::any_of(kSourceFormats.begin(), kSourceFormats.end(),
                       [&](const SourceFormat& f) { return ext == f.extension; });
}

bool IsSourceKind(const std::string& ext, SourceKind kind) {
    return std::any_of(kSourceFormats.begin(), kSourceFormats.end(), [&](const SourceFormat& f) {
        return f.kind == kind && ext == f.extension;
    });
}

SourceKind KindOf(const std::string& ext) {
    for (const SourceFormat& f : kSourceFormats)
        if (ext == f.extension) return f.kind;
    return SourceKind::Image; // caller should gate on IsSourceFormat first
}

bool IsPackable(const std::string& ext) {
    return std::any_of(kEngineAssets.begin(), kEngineAssets.end(), [&](const EngineAsset& a) {
        return a.runtimeLoaded && ext == a.extension;
    });
}

std::wstring BuildImportDialogFilter() {
    // Win32 filters are "Label\0patterns\0...\0\0" - embedded NULs, so build the
    // std::wstring explicitly rather than from a literal.
    const auto patternsFor = [](SourceKind k) {
        std::wstring out;
        for (const SourceFormat& f : kSourceFormats) {
            if (f.kind != k) continue;
            if (!out.empty()) out += L';';
            out += L'*';
            const std::string e = f.extension;
            out.append(e.begin(), e.end()); // extensions are ASCII
        }
        return out;
    };
    const std::wstring images = patternsFor(SourceKind::Image);
    const std::wstring models = patternsFor(SourceKind::Model);
    const std::wstring audio = patternsFor(SourceKind::Audio);
    const std::wstring fonts = patternsFor(SourceKind::Font);
    std::wstring all = images;
    for (const std::wstring* g : {&models, &audio, &fonts}) {
        if (g->empty()) continue;
        if (!all.empty()) all += L';';
        all += *g;
    }

    std::wstring filter;
    const auto add = [&filter](const wchar_t* label, const std::wstring& patterns) {
        if (patterns.empty()) return;
        filter += label;
        filter += L'\0';
        filter += patterns;
        filter += L'\0';
    };
    add(L"All supported assets", all);
    add(L"Images", images);
    add(L"Models", models);
    add(L"Audio", audio);
    add(L"Fonts", fonts);
    add(L"All files", L"*.*");
    filter += L'\0'; // terminating empty entry
    return filter;
}

} // namespace hbe::assets
