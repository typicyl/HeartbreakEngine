// Assets/AssetRefs.cpp - see AssetRefs.h for the design and the reasoning.
#include "Assets/AssetRefs.h"

#include "Assets/AssetFormats.h"
#include "Assets/Mesh.h"
#include "Assets/UAF.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>

namespace hbe::assets {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string StatusName(RefStatus s, const std::string& key) {
    switch (s) {
        case RefStatus::Resolved:  return "resolved";
        case RefStatus::Ambiguous: return "ambiguous filename";
        case RefStatus::CaseMismatch:
            // Spelled out, because this one looks like a non-problem in the editor:
            // NTFS resolves it, so nothing is visibly wrong until the shipped build.
            return "WRONG CASE - the file on disk is '" + key +
                   "'. The editor reads it anyway (Windows is case-insensitive) but the "
                   "packed runtime matches pack keys byte-for-byte, so this asset would be "
                   "silently missing in the shipped build ONLY. Fix the spelling";
        default: return "not found";
    }
}

// Recursively pushes every STRING value in a JSON document. Field names are
// deliberately ignored: the whole point of the value scan is that it cannot go
// stale when a field is added or renamed. `depth` guards against a hand-edited
// document nested deeply enough to blow the stack.
void ScanJsonStrings(const nlohmann::json& j, std::vector<std::string>& out, u32 depth) {
    if (depth > 64) return;
    if (j.is_string()) {
        out.push_back(j.get<std::string>());
        return;
    }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) ScanJsonStrings(it.value(), out, depth + 1);
        return;
    }
    if (j.is_array()) {
        for (const nlohmann::json& v : j) ScanJsonStrings(v, out, depth + 1);
    }
}

// Extracts the raw outbound references of one file per its registry row.
// Returns false when the file could not be read/parsed at all, or when its
// extension has no RefScan - both mean the closure cannot be proven total, and
// the caller records the file as unreadable rather than assuming it is a leaf.
bool CollectRefsOf(const fs::path& file, const std::string& ext, std::vector<std::string>& out) {
    switch (RefScanOf(ext)) {
        case RefScan::Leaf:
            return true;
        case RefScan::Hook: {
            const CollectRefsFn fn = CollectorOf(ext);
            if (!fn) return false; // --test-assetformats exists to make this unreachable
            return fn(file, out);
        }
        case RefScan::JsonScan: {
            const std::optional<std::vector<u8>> bytes = vfs::ReadFile(file);
            if (!bytes || bytes->empty()) return false;
            // HISTORICAL NOTE, because this catch was once blamed for a crash it
            // did not cause. A malformed document used to abort the DEBUG editor
            // here with _CrtIsValidHeapPointer, and the cause was recorded as "a
            // defect in nlohmann/json 3.11.3". That was wrong. The real cause was
            // build-wide: JoltPhysics pushed _HAS_EXCEPTIONS=0 onto every engine TU
            // while the executables compiled with the default 1, so std::exception
            // was 16 bytes on one side of the library boundary and 24 on the other
            // and every throw/catch in the engine freed a pointer the CRT never
            // handed out. Nothing in nlohmann, and nothing in this function.
            // cmake/Dependencies.cmake fixes it; Core/Types.h #errors if it comes
            // back. Left here because "the JSON parser is unsafe in Debug" was a
            // load-bearing false belief for a while.
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(bytes->begin(), bytes->end());
            } catch (const std::exception&) {
                return false;
            }
            ScanJsonStrings(j, out, 0);
            return true;
        }
        default:
            return false; // Unspecified
    }
}

} // namespace

bool CollectFileRefs(const fs::path& file, std::vector<std::string>& out) {
    return CollectRefsOf(file, NormalizeExtension(file), out);
}

// --- RefScan::Hook collectors (declared in AssetFormats.h) ------------------

bool CollectRefsUaf(const fs::path& file, std::vector<std::string>& out) {
    // Only MESH assets carry references. Peek the fixed header rather than paging
    // in a texture payload that can be hundreds of megabytes just to find out.
    // (PeekType is a bounded 20-byte DISK read - the cook always runs against the
    // loose project, never a mounted pack.)
    const uaf::AssetType type = uaf::PeekType(file);
    if (type == uaf::AssetType::Unknown) return false; // corrupt / not a .uaf
    if (type != uaf::AssetType::Mesh) return true;     // texture / audio / font: leaf
    const std::optional<Model> model = uaf::ReadMesh(file);
    if (!model) return false;
    for (const MeshData& md : *model) {
        // `materialAsset` is the field BOTH previous add-sites dropped: the mesh
        // was packed, its generated `.hbmat` was not, and the entity rendered
        // with import-time factors instead of the authored material.
        for (const std::string* s :
             {&md.material.baseColorTex, &md.material.normalTex, &md.material.mrTex,
              &md.material.aoTex, &md.material.emissiveTex, &md.material.materialAsset}) {
            if (!s->empty()) out.push_back(*s);
        }
    }
    return true;
}

bool CollectRefsFracture(const fs::path& file, std::vector<std::string>& out) {
    // Header only: 'FRAC' | u32 version | vec3 boundsMin | vec3 boundsMax |
    // u32 len + interiorMaterial bytes. Loading the chunk soup to read one string
    // would mean decompressing every fragment mesh in the project on every cook.
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(file);
    if (!bytes) return false;
    constexpr u32 kFracMagic = 0x43415246u; // 'FRAC' - mirrors Assets/Fracture.cpp
    constexpr usize kStrOffset = 4 + 4 + 12 + 12;
    if (bytes->size() < kStrOffset + 4) return false;
    u32 magic = 0;
    std::memcpy(&magic, bytes->data(), 4);
    if (magic != kFracMagic) return false;
    u32 len = 0;
    std::memcpy(&len, bytes->data() + kStrOffset, 4);
    if (len > 4096 || bytes->size() < kStrOffset + 4 + len) return false;
    if (len != 0)
        out.emplace_back(reinterpret_cast<const char*>(bytes->data() + kStrOffset + 4), len);
    return true;
}

// --- ReferenceResolver ------------------------------------------------------

bool ReferenceResolver::LooksLikeAssetRef(const std::string& raw, std::string* outRel) {
    // A path longer than this is not a path anyone authored; refusing it keeps a
    // pathological document from turning the scan into a filesystem crawl.
    if (raw.empty() || raw.size() > 1024) return false;
    std::string s = raw;

    // Scene mesh provenance is "uaf:<rel>#<submesh>".
    if (s.rfind("uaf:", 0) == 0) s = s.substr(4);
    if (const usize hash = s.find_last_of('#'); hash != std::string::npos) {
        bool allDigits = hash + 1 < s.size();
        for (usize i = hash + 1; allDigits && i < s.size(); ++i)
            allDigits = std::isdigit(static_cast<unsigned char>(s[i])) != 0;
        if (allDigits) s = s.substr(0, hash);
    }
    for (char& c : s)
        if (c == '\\') c = '/';
    while (s.rfind("./", 0) == 0) s = s.substr(2);
    if (s.empty() || s.back() == '/') return false;
    if (s.find_first_of("\r\n\t*?|<>") != std::string::npos) return false;

    if (!IsPackable(NormalizeExtension(fs::path(s)))) return false;
    if (outRel) *outRel = s;
    return true;
}

ReferenceResolver::ReferenceResolver(const fs::path& assetsDir) : assetsDir_(assetsDir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (!IsPackable(NormalizeExtension(it->path()))) continue;
        const std::string rel = fs::relative(it->path(), assetsDir, ec).generic_string();
        if (ec || rel.empty()) continue;
        const std::string name = it->path().filename().generic_string();
        exactRel_.insert(rel);
        byExactName_[name].push_back(rel);
        byRel_[Lower(rel)] = rel;
        byName_[Lower(name)].push_back(rel);
    }
}

ResolvedRef ReferenceResolver::Resolve(const std::string& raw) const {
    std::string rel;
    if (!LooksLikeAssetRef(raw, &rel)) return {RefStatus::NotFound, {}};

    // An absolute path that happens to sit under Assets/ is still a reference to
    // that asset (older importers wrote them; Project::RelativeAssetPath does the
    // same reduction).
    if (const fs::path p(rel); p.is_absolute()) {
        std::error_code ec;
        const fs::path r = fs::relative(p, assetsDir_, ec);
        if (!ec && !r.empty() && !r.native().starts_with(L"..")) rel = r.generic_string();
    }
    const std::string norm = fs::path(rel).lexically_normal().generic_string();
    // BYTE-EXACT FIRST, ALWAYS. uap::PackSet::Read hashes the pack key and
    // vfs::ReadByFilename compares filenames with ==, so a reference that differs
    // from the file on disk by so much as one letter's case is not resolvable at
    // runtime - and resolving it here would pack the asset under a key nothing
    // ever asks for.
    if (exactRel_.count(norm)) return {RefStatus::Resolved, norm};

    // Filename fallback - the same rule vfs::ReadByFilename applies at runtime,
    // except that here two matches is an ERROR rather than "take whichever entry
    // the pack scan hits first".
    const std::string name = fs::path(norm).filename().generic_string();
    if (auto it = byExactName_.find(name); it != byExactName_.end()) {
        if (it->second.size() == 1) return {RefStatus::Resolved, it->second.front()};
        return {RefStatus::Ambiguous, {}};
    }

    // Nothing matches byte-for-byte. Before saying "not found" - which on a
    // case-insensitive filesystem is confusing, because the author can see the file
    // sitting right there and the editor loads it fine - check whether the only
    // difference is case, and if so say exactly that.
    if (auto it = byRel_.find(Lower(norm)); it != byRel_.end())
        return {RefStatus::CaseMismatch, it->second};
    if (auto it = byName_.find(Lower(name)); it != byName_.end() && it->second.size() == 1)
        return {RefStatus::CaseMismatch, it->second.front()};
    return {RefStatus::NotFound, {}};
}

// --- ComputeClosure ---------------------------------------------------------

ClosureResult ComputeClosure(const fs::path& assetsDir, const ClosureOptions& options) {
    ClosureResult result;
    std::error_code ec;
    if (!fs::exists(assetsDir, ec)) {
        HBE_ERROR("Closure: assets directory '{}' does not exist.", assetsDir.string());
        return result;
    }
    const ReferenceResolver resolver(assetsDir);

    std::deque<std::string> work;
    const auto push = [&](const std::string& key) {
        if (!result.included.insert(key).second) return;
        work.push_back(key);
        // P6: a texture's BC variant `<stem>.bc.uaf` is generated beside `<stem>.uaf` but is
        // referenced by NOBODY (materials name the uncompressed `.uaf`). Pack it whenever its
        // source is included, or a "pack only referenced" cook drops it and the runtime BC path
        // silently falls back to uncompressed in the shipped build - the VRAM win would evaporate.
        // Only a texture actually HAS a sibling on disk; a mesh `.uaf` simply won't match fs::exists.
        if (key.ends_with(".uaf") && !key.ends_with(".bc.uaf")) {
            const std::string bc = key.substr(0, key.size() - 4) + ".bc.uaf";
            std::error_code bcec;
            if (fs::exists(assetsDir / bc, bcec) && result.included.insert(bc).second)
                work.push_back(bc);
        }
    };
    // One report line per (file, string), not per occurrence: a texture missing
    // from 400 entities is one problem, not 400.
    std::set<std::string> missingSeen;
    const auto reportMissing = [&](const std::string& from, const std::string& raw,
                                   const ResolvedRef& rr) {
        if (!missingSeen.insert(from + '\x1f' + raw).second) return;
        result.missing.push_back({from, raw, StatusName(rr.status, rr.key)});
    };

    // --- Roots ---------------------------------------------------------------
    for (const std::string& raw : options.roots) {
        if (raw.empty()) continue;
        const ResolvedRef rr = resolver.Resolve(raw);
        if (rr.status == RefStatus::Resolved) {
            push(rr.key);
        } else {
            reportMissing("<project settings>", raw, rr);
        }
    }
    if (options.sweepEntryPoints) {
        for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            const std::string ext = NormalizeExtension(it->path());
            if (ext != ".hbscene" && ext != ".hbui" && ext != ".hbprefab") continue;
            const std::string rel = fs::relative(it->path(), assetsDir, ec).generic_string();
            if (!ec && !rel.empty()) push(rel);
        }
    }
    result.rootCount = static_cast<u32>(result.included.size());

    // --- Worklist to fixpoint ------------------------------------------------
    std::vector<std::string> raws;
    while (!work.empty()) {
        const std::string key = work.front();
        work.pop_front();
        raws.clear();
        if (!CollectRefsOf(assetsDir / fs::path(key), NormalizeExtension(fs::path(key)), raws)) {
            result.unreadable.push_back(key);
            continue;
        }
        for (const std::string& raw : raws) {
            if (!ReferenceResolver::LooksLikeAssetRef(raw)) continue; // gate 1
            const ResolvedRef rr = resolver.Resolve(raw);             // gate 2
            if (rr.status == RefStatus::Resolved) {
                push(rr.key);
            } else {
                reportMissing(key, raw, rr);
            }
        }
    }

    // --- Included / excluded accounting --------------------------------------
    for (const std::string& key : result.included)
        ++result.includedByExt[NormalizeExtension(fs::path(key))];
    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const std::string ext = NormalizeExtension(it->path());
        if (!IsPackable(ext)) continue;
        const std::string rel = fs::relative(it->path(), assetsDir, ec).generic_string();
        if (ec || rel.empty() || result.included.count(rel)) continue;
        result.excluded.push_back(rel);
        ++result.excludedByExt[ext];
    }
    std::sort(result.excluded.begin(), result.excluded.end());

    result.ok = result.missing.empty() && result.unreadable.empty();
    return result;
}

void LogClosureReport(const ClosureResult& result, bool onlyReferenced, bool allowMissingRefs) {
    const auto breakdown = [](const std::map<std::string, u32>& m) {
        std::string s;
        for (const auto& [ext, n] : m) {
            if (!s.empty()) s += ", ";
            s += ext + " x" + std::to_string(n);
        }
        return s.empty() ? std::string("none") : s;
    };

    HBE_INFO("Closure: {} root(s) -> INCLUDED {} asset(s) [{}]", result.rootCount,
             result.included.size(), breakdown(result.includedByExt));
    if (!onlyReferenced) {
        HBE_INFO("Closure: 'Pack only referenced' is OFF - every packable asset ships "
                 "regardless; the {} asset(s) below are reported for information only.",
                 result.excluded.size());
    }
    HBE_INFO("Closure: EXCLUDED {} asset(s) [{}]{}", result.excluded.size(),
             breakdown(result.excludedByExt),
             (onlyReferenced && !result.excluded.empty())
                 ? " - these will NOT be in the shipped packs"
                 : "");
    // The exclusion list is the deliverable that makes an exclusion PROVABLE
    // rather than assumed, so print it (capped - the point is reviewability).
    constexpr usize kMaxListed = 40;
    for (usize i = 0; i < result.excluded.size() && i < kMaxListed; ++i)
        HBE_INFO("Closure:   excluded  {}", result.excluded[i]);
    if (result.excluded.size() > kMaxListed)
        HBE_INFO("Closure:   ... and {} more.", result.excluded.size() - kMaxListed);

    // Severity follows CONSEQUENCE, not opinion. A dangling reference only blocks
    // the cook when the filter is actually driving what ships; with the filter
    // off it is still a real authoring bug worth saying out loud, but it changes
    // nothing about this build, so it must not read as a build failure.
    const bool blocking = onlyReferenced && !allowMissingRefs;
    const auto say = [blocking](const std::string& msg) {
        if (blocking) {
            HBE_ERROR("{}", msg);
        } else {
            HBE_WARN("{}", msg);
        }
    };
    for (const std::string& f : result.unreadable) {
        say("Closure: '" + f +
            "' is reachable but could not be read or parsed - the closure below it is "
            "UNKNOWN, so anything it references may be missing from the packs. Fix or "
            "delete the file.");
    }
    for (const MissingRef& m : result.missing) {
        // Not a packing problem: the file does not exist anywhere, so the game is
        // already broken. The only question is whether anyone hears about it.
        say("Closure: '" + m.from + "' references '" + m.raw + "' - " + m.reason + ".");
    }
    if (result.ok) return;
    if (blocking) {
        HBE_ERROR("Closure: {} unresolvable reference(s) and {} unreadable file(s). "
                  "Refusing to cook: a filtered pack built from a closure that could not be "
                  "proven total is guesswork, and every one of these failures is SILENT at "
                  "runtime. Clear the stale references, or set \"allowMissingRefs\": true in "
                  "the .hbproj build block to downgrade this to a warning.",
                  result.missing.size(), result.unreadable.size());
    } else if (onlyReferenced) {
        HBE_WARN("Closure: {} unresolvable reference(s) and {} unreadable file(s); "
                 "BuildSettings.allowMissingRefs is ON, so the cook continues. The shipped "
                 "build WILL be missing whatever those references named.",
                 result.missing.size(), result.unreadable.size());
    } else {
        HBE_WARN("Closure: {} unresolvable reference(s) and {} unreadable file(s). 'Pack only "
                 "referenced' is OFF so this build is unaffected - but these are dangling "
                 "references in the project, and they would BLOCK the cook the moment the "
                 "filter is turned on.",
                 result.missing.size(), result.unreadable.size());
    }
}

} // namespace hbe::assets
