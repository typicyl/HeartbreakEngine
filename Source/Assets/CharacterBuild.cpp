// Assets/CharacterBuild.cpp
#include "Assets/CharacterBuild.h"

#include "Assets/CharacterAsset.h"
#include "Assets/UAF.h"
#include "Core/Log.h"

#include <cstdlib>
#include <optional>
#include <vector>

namespace hbe::assets {

namespace fs = std::filesystem;

namespace {

// Splits "uaf:<rel>#<n>" into the relative path and submesh index (0 if absent).
bool SplitUaf(const std::string& source, std::string& rel, u32& submesh) {
    if (source.rfind("uaf:", 0) != 0) return false;
    std::string rest = source.substr(4);
    submesh = 0;
    if (const usize hash = rest.find_last_of('#'); hash != std::string::npos) {
        submesh = static_cast<u32>(std::strtoul(rest.c_str() + hash + 1, nullptr, 10));
        rest = rest.substr(0, hash);
    }
    rel = rest;
    return !rel.empty();
}

} // namespace

BuiltCharacter BuildCharacter(const fs::path& assetsDir, const CharacterAsset& c) {
    BuiltCharacter out;

    // --- Canonical skeleton (drives every part) ---
    std::string skRel;
    u32 skSub = 0;
    if (!SplitUaf(c.skeleton, skRel, skSub)) {
        HBE_ERROR("Character: invalid skeleton source '{}'.", c.skeleton);
        return out;
    }
    const std::optional<Rig> skRig = uaf::ReadRig(assetsDir / skRel);
    if (!skRig || !skRig->Valid()) {
        HBE_ERROR("Character: skeleton asset '{}' has no rig.", skRel);
        return out;
    }
    out.skeleton = skRig->skeleton;
    out.skeletonSource = c.skeleton;

    // --- Load + remap every variant's part mesh onto the canonical skeleton ---
    // Store meshes in the map first so weld::Part can hold stable pointers into it.
    for (const CharacterVariant& v : c.variants) {
        std::string rel;
        u32 sub = 0;
        if (!SplitUaf(v.mesh, rel, sub)) {
            HBE_WARN("Character: variant '{}' has invalid mesh '{}'.", v.id, v.mesh);
            continue;
        }
        const std::optional<Model> model = uaf::ReadMesh(assetsDir / rel);
        if (!model || sub >= model->size()) {
            HBE_WARN("Character: variant '{}' mesh '{}' (submesh {}) failed to load.", v.id, rel, sub);
            continue;
        }
        MeshData mesh = (*model)[sub];

        // Remap the part's joint indices onto the canonical skeleton by bone name.
        // If the part ships its own rig (a separate outfit .uaf), this rebinds it;
        // parts authored in the skeleton file itself remap to identity (no-op).
        if (const std::optional<Rig> partRig = uaf::ReadRig(assetsDir / rel);
            partRig && partRig->Valid()) {
            const std::vector<i32> remap = weld::BuildJointRemap(partRig->skeleton, out.skeleton);
            weld::RemapJoints(mesh, remap);
        }
        out.welded.emplace(v.id, std::move(mesh));
    }

    if (out.welded.empty()) {
        HBE_ERROR("Character: no variant meshes loaded.");
        return out;
    }

    // --- Weld seams across ALL variants (loadout-independent solidity) ---
    // Each slot's DEFAULT variant is the canonical binding source at its seams.
    std::vector<weld::Part> parts;
    parts.reserve(out.welded.size());
    for (const CharacterVariant& v : c.variants) {
        const auto it = out.welded.find(v.id);
        if (it == out.welded.end()) continue;
        weld::Part p;
        p.mesh = &it->second; // unordered_map node addresses are stable
        p.seamMode = v.seamMode;
        p.isMaster = (c.DefaultVariant(v.slot) == v.id);
        parts.push_back(p);
    }
    out.stats = weld::WeldSeams(parts, 1e-4f, &out.openBoundaryPositions);
    out.ok = true;

    HBE_INFO("Character built: {} variant(s), {} seam group(s), {} verts welded, "
             "{} open-boundary, {} non-manifold edge(s), tol={}.",
             out.welded.size(), out.stats.groups, out.stats.weldedVertices,
             out.stats.openBoundary, out.stats.nonManifoldEdges, out.stats.tolerance);
    return out;
}

} // namespace hbe::assets
