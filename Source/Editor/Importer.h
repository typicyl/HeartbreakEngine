// Editor/Importer.h - converts source assets (images, models, audio) to .uaf.
//
// Editor-only: a shipped game never imports raw files, it loads the resulting
// `.uaf` assets directly.
#pragma once

#include <filesystem>
#include <optional>

namespace hbe::importer {

// True if `extension` (including the dot, lower-case ok) is importable.
bool IsSupportedSource(const std::filesystem::path& path);

// Imports `src` into `assetsDir`, producing a `<stem>.uaf`. Returns the created
// path on success.
std::optional<std::filesystem::path> Import(const std::filesystem::path& src,
                                            const std::filesystem::path& assetsDir);

} // namespace hbe::importer
