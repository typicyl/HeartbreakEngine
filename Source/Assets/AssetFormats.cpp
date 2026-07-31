// Assets/AssetFormats.cpp
#include "Assets/AssetFormats.h"

#include "Core/Log.h"

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
// The `scan` column is the SECOND shipping contract this table carries (the
// first is `runtimeLoaded`). It tells the dependency-closure walker how to find
// what a file points AT, which is what makes "pack only referenced assets"
// total instead of a hand-maintained walk that drifts. JsonScan is the default
// and correct answer for every JSON format: it needs no field list, so a new
// field (or a whole new node type) is covered with zero edits here. Reach for
// Hook only when there is no JSON to scan, and for Leaf only when the format
// provably carries no outbound path at all - and say why in `note`, because
// --test-assetformats demands it.
//
// The `scan` column also decides, without a third list, WHERE a format keeps its
// PACK SLOT (Assets/SlotIds.h): `.uaf` has a header field, every JsonScan format
// takes a top-level "packSlot" key, and the rest - the binary bakes - keep theirs
// in the `.uapmanifest` instead, because their producers rewrite the whole file
// on every bake and would destroy an embedded field as fast as it was written.
// slots::CanEmbedSlot is derived from exactly that, so a new row picks its slot
// home by picking its scan.
const std::vector<EngineAsset> kEngineAssets = {
    {".uaf",        "Asset",            true,  RefScan::Hook,     &CollectRefsUaf,
     "meshes / textures / audio / fonts; a MESH names its material textures + .hbmat"},
    {".hbscene",    "Scene",            true,  RefScan::JsonScan, nullptr,
     "also the format of .hbprefab subtrees"},
    {".hbmat",      "Material",         true,  RefScan::JsonScan, nullptr, ""},
    {".hbpaint",    "Paint Canvas",     true,  RefScan::Leaf,     nullptr,
     "Art Editor surface paint; binary layer pixels only, names no other asset"},
    {".hbevent",    "Audio Event",      true,  RefScan::JsonScan, nullptr, ""},
    {".hbschem",    "Schematic",        true,  RefScan::JsonScan, nullptr,
     "visual script; asset paths are node LITERALS, found by the value scan"},
    {".hbdialogue", "Dialogue",         true,  RefScan::JsonScan, nullptr,
     "branching conversation graph"},
    {".hbcutscene", "Cutscene",         true,  RefScan::JsonScan, nullptr, ""},
    {".hbmusic",    "Music Graph",      true,  RefScan::JsonScan, nullptr, "adaptive music"},
    {".hbchar",     "Character",        true,  RefScan::JsonScan, nullptr,
     "modular rig; CharacterSystem loads it via the VFS"},
    {".hbprefab",   "Prefab",           true,  RefScan::JsonScan, nullptr,
     "SpawnSystem instantiates these at runtime"},
    {".hbuianim",   "UI Animation",     true,  RefScan::JsonScan, nullptr, "UIAnimator clip"},
    {".hbui",       "UI Document",      true,  RefScan::JsonScan, nullptr,
     "screen/world UI tree; ui::LoadDocument reads it via the VFS"},
    {".hbgi",       "GI Volume",        true,  RefScan::Leaf,     nullptr,
     "baked irradiance cache (SceneEnvironment::giSource); binary SH atlas, no paths"},
    {".hbprobe",    "Probe Cache",      true,  RefScan::Leaf,     nullptr,
     "baked local env maps (ReflectionProbe::source); binary pixels, no paths. Was "
     "absent from this table entirely, so IsPackable said false and every probe "
     "silently failed to ship (SceneSerializer just skips an invalid bake)"},
    {".hbfrac",     "Fracture",         true,  RefScan::Hook,     &CollectRefsFracture,
     "pre-fractured destructible chunks + adjacency; header names an interior .hbmat"},
    // NOT packed by extension - each for a specific reason. Do not "fix" these to
    // true: .hbproj IS read at runtime through the VFS, but it ships via the
    // separate uap::ExtraFile path (BuildShipping packs it under the virtual name
    // "__project.hbproj"), so listing it here would pack a second, wrongly-named
    // copy. .hbsave lives in the user's save directory and is written at runtime.
    //
    // .hbcharcache was runtimeLoaded=true, which was a LIE in the single source of
    // truth: nothing in the tree writes one (CharacterBuild welds in memory and
    // returns) and nothing loads one. A registered runtime format that cannot
    // exist has no answer to "how does the closure walk it", so it is recorded
    // here as what it actually is - a reserved extension.
    {".hbcharcache","Character Cache",  false, RefScan::Leaf,     nullptr,
     "reserved: derived seam-welded mesh for a .hbchar. No writer and no loader "
     "exists yet, so nothing can reference it and nothing can load it"},
    {".hbproj",     "Project",          false, RefScan::JsonScan, nullptr,
     "packed as an ExtraFile (__project.hbproj); its asset fields seed the closure roots"},
    {".hbsave",     "Save Game",        false, RefScan::Leaf,     nullptr,
     "written at runtime into the user's save dir; never packed, never a closure root"},
    {".uapmanifest","Pack Manifest",    false, RefScan::Leaf,     nullptr,
     "editor-side slot bookkeeping; contains pack keys, not asset references"},
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

namespace {
const EngineAsset* FindAsset(const std::string& ext) {
    for (const EngineAsset& a : kEngineAssets)
        if (ext == a.extension) return &a;
    return nullptr;
}
} // namespace

RefScan RefScanOf(const std::string& ext) {
    const EngineAsset* a = FindAsset(ext);
    return a ? a->scan : RefScan::Unspecified;
}

CollectRefsFn CollectorOf(const std::string& ext) {
    const EngineAsset* a = FindAsset(ext);
    return (a && a->scan == RefScan::Hook) ? a->collect : nullptr;
}

bool RegistrySelfTest() {
    u32 failures = 0;
    const auto fail = [&](const std::string& why) {
        HBE_ERROR("assetformats: {}", why);
        ++failures;
    };

    std::vector<std::string> seen;
    for (const EngineAsset& a : kEngineAssets) {
        const std::string ext = a.extension ? a.extension : "";
        if (ext.size() < 2 || ext.front() != '.') {
            fail("row '" + ext + "' is not a well-formed extension (want \".xyz\").");
            continue;
        }
        if (ext != NormalizeExtension(std::filesystem::path(ext)))
            fail("row '" + ext + "' is not lower-case; every lookup normalises first.");
        if (std::find(seen.begin(), seen.end(), ext) != seen.end())
            fail("extension '" + ext + "' appears twice; lookups would silently take the first.");
        seen.push_back(ext);
        if (!a.name || !*a.name) fail("row '" + ext + "' has no display name.");
        if (!a.note) fail("row '" + ext + "' has a null note.");

        // THE completeness assertion: a format the runtime loads must declare how
        // the closure walks it. Unspecified means "nobody decided", and the cost
        // of not deciding is a shipped build missing assets with no diagnostic.
        if (a.runtimeLoaded && a.scan == RefScan::Unspecified)
            fail("'" + ext +
                 "' is runtimeLoaded but its RefScan is Unspecified - the pack closure "
                 "cannot walk it, so anything it references would silently not ship. "
                 "Pick JsonScan (any JSON format), Hook (+ a collector), or Leaf.");
        if (a.scan == RefScan::Hook && a.collect == nullptr)
            fail("'" + ext + "' declares RefScan::Hook but has no collector function.");
        if (a.scan != RefScan::Hook && a.collect != nullptr)
            fail("'" + ext + "' has a collector but is not RefScan::Hook; it would never run.");
        if (a.scan == RefScan::Leaf && (!a.note || !*a.note))
            fail("'" + ext +
                 "' declares RefScan::Leaf with no justification in `note`. Write down WHY "
                 "it carries no outbound reference, so the next reader can check the claim.");

        // Lookup helpers must agree with the row (they are what consumers call).
        if (IsPackable(ext) != a.runtimeLoaded)
            fail("IsPackable('" + ext + "') disagrees with the row's runtimeLoaded.");
        if (RefScanOf(ext) != a.scan) fail("RefScanOf('" + ext + "') disagrees with the row.");
        if (CollectorOf(ext) != (a.scan == RefScan::Hook ? a.collect : nullptr))
            fail("CollectorOf('" + ext + "') disagrees with the row.");
    }

    // A source format must never also be an engine asset: the importer would try
    // to convert a cooked file, and the cooker would try to pack a raw one.
    for (const SourceFormat& s : kSourceFormats)
        if (FindAsset(s.extension))
            fail(std::string("'") + s.extension +
                 "' is registered as BOTH a source format and an engine asset.");

    // Unknown extensions must be inert, not accidentally packable/scannable.
    if (IsPackable(".not-a-real-ext") || RefScanOf(".not-a-real-ext") != RefScan::Unspecified ||
        CollectorOf(".not-a-real-ext") != nullptr)
        fail("an unregistered extension is not inert.");

    if (failures != 0) {
        HBE_ERROR("assetformats: {} registry violation(s).", failures);
        return false;
    }
    HBE_INFO("assetformats: {} engine asset row(s), {} source format(s) - all invariants hold.",
             kEngineAssets.size(), kSourceFormats.size());
    return true;
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
