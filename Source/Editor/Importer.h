// Editor/Importer.h - converts source assets (images, models, audio) to .uaf.
//
// Editor-only: a shipped game never imports raw files, it loads the resulting
// `.uaf` assets directly.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>

namespace hbe::importer {

// True if `extension` (including the dot, lower-case ok) is importable.
bool IsSupportedSource(const std::filesystem::path& path);

// Imports `src` into `assetsDir`, producing a `<stem>.uaf`. Returns the created
// path on success.
std::optional<std::filesystem::path> Import(const std::filesystem::path& src,
                                            const std::filesystem::path& assetsDir);

// Summary of an auto-upgrade pass.
struct UpgradeReport {
    u32 scanned = 0;      // .uaf files inspected
    u32 meshesUpgraded = 0; // meshes below the current version, re-written at it (+ LODs)
    u32 texturesBaked = 0;  // BC variants freshly baked for material textures
    bool changedAnything() const { return meshesUpgraded > 0 || texturesBaked > 0; }
};

// Brings every `.uaf` under `assetsDir` up to the CURRENT format spec, IN PLACE and
// NON-DESTRUCTIVELY: the `.uaf` is a build artifact of the source glTF/PNG, and each asset's
// IDENTITY (guid, rig, pack slot) is preserved (guid+rig round-tripped through WriteMesh; the slot
// re-assigned from the manifest, which is keyed by path). Meshes below the current payload version
// are re-read, re-LOD'd (when meshLodEnabled + eligible), and re-written at the current version.
// When textureCompression is on, material textures gain a role-correct `.bc.uaf` sibling if they
// lack one. IDEMPOTENT: a current asset is only peeked, never rewritten. `manifestPath` is the
// project's slot manifest (Project::SlotManifestPath()). Editor-only.
UpgradeReport UpgradeAssets(const std::filesystem::path& assetsDir,
                            const std::filesystem::path& manifestPath);

bool UpgradeSelfTest(); // --test-upgrade: a forced-old mesh migrates to v9 keeping guid + slot

} // namespace hbe::importer
