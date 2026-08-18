// Assets/CookStats.h - "why is this game N GB?" cook analysis.
//
// Cooks a project's Assets into packs in a TEMP dir (never touching the real build) and
// reports where the bytes go: per-asset-type source (loose .uaf) size vs packed size and
// the compression ratio, within-pack dedup savings, the pack files' sizes, and the single
// largest assets. This is the tool that turns "the build is huge" into an actual answer.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace hbe::cookstats {

// Cooks `assetsDir` (seeding slots from a COPY of `manifestPath`, so the real ledger is
// untouched) and writes a human-readable report into `outText`. Returns false only if the
// cook itself fails. `label` names the run in the report header.
bool Report(const std::filesystem::path& assetsDir, const std::filesystem::path& manifestPath,
            const std::string& label, std::string& outText);

// --test-cookstats: synthesizes a tiny project (a texture, a mesh, an audio clip and a
// byte-identical duplicate), runs Report, and asserts the report is well-formed (all
// categories present, a real compression ratio, dedup detected). Headless.
bool SelfTest();

} // namespace hbe::cookstats
