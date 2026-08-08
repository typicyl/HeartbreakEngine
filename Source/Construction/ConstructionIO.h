// Construction/ConstructionIO.h - the `.hbbuild` asset.
//
// WHAT IS STORED, and why it is BOTH the preset and the graph.
//
// A procedural tool has a standing tension: the preset plus its parameters is the compact,
// re-editable source, but manual overrides (brief SS18 - move a component, delete one, add custom
// geometry, lock a section) make the actual graph diverge from anything the preset would produce.
// Storing only the parameters silently discards every override on the next load. Storing only the
// graph loses the parameter UI, so the artist can never turn the dial again.
//
// So a .hbbuild carries both:
//
//     presetId + PresetParams   the dials, so the inspector still works
//     ConstructionDef           the actual graph, INCLUDING overrides
//
// On load the DEFINITION wins - it is what the artist last saw. Re-running the preset is an
// explicit action that says "throw my overrides away and rebuild from the parameters", and the
// editor asks before doing it rather than doing it on load.
//
// SHIPPING CONTRACT. `.hbbuild` is registered runtimeLoaded in Assets/AssetFormats.cpp, so the
// pack cooker includes it. A runtime-loaded format left out of the packs disappears in a shipped
// build with NO error at all - four formats in this engine have already died that way. And every
// runtime read goes through vfs::ReadFile, never std::ifstream: a shipped build serves assets from
// mounted packs, not from loose files, and that is the second, independent way the same features
// died.
#pragma once

#include "Construction/ConstructionPreset.h"

#include <filesystem>
#include <string>

namespace hbe::construction {

// One authored procedural structure.
struct BuildAsset {
    std::string presetId = "wall";
    PresetParams params{};
    ConstructionDef def{};
    DamageState damage{}; // persistent destruction (brief SS15) - id-keyed and tiny
    f32 chunkSize = 4.0f;
};

// Text JSON, matching every other authored asset in this engine. Binary would be smaller and
// would also make a .hbbuild unmergeable and undiffable, which for a source asset under version
// control is the wrong trade - the whole point of a parametric definition is that it is small.
bool SaveBuild(const std::filesystem::path& file, const BuildAsset& asset, std::string& outError);

// Editor-side load (loose files on disk).
bool LoadBuild(const std::filesystem::path& file, BuildAsset& out, std::string& outError);

// RUNTIME load. Goes through the VFS so a shipped build reads it from the mounted packs.
bool LoadBuildVfs(const std::string& relPath, BuildAsset& out, std::string& outError);

// Serialize to / from a string, for the collaboration journal and for undo snapshots.
std::string BuildToString(const BuildAsset& asset);
bool BuildFromString(const std::string& text, BuildAsset& out, std::string& outError);

bool IoSelfTest();

} // namespace hbe::construction
