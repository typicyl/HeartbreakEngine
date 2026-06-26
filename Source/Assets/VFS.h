// Assets/VFS.h - pack-aware virtual file system for asset reads.
//
// In the editor (and in unpacked builds) assets are loose files under the
// project's Assets/ directory and every read falls through to disk. A shipped
// build mounts its `.uap` packs instead: reads of paths under the mounted
// virtual root (the project's Assets/ dir) are served from the packs, so the
// shipping folder doesn't need the loose files at all.
//
// All asset loaders (UAF, scenes, materials) route their reads through here.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe::vfs {

// Mounts every `<baseName>_<n>.uap` chunk in `packDir`. Paths under
// `virtualRoot` resolve to pack entries by their Assets-relative path.
// Returns false (and stays unmounted) when no packs are found.
bool MountPacks(const std::filesystem::path& packDir, const std::string& baseName,
                const std::filesystem::path& virtualRoot);
void Unmount();
bool IsMounted();

// Last-chance resolution root: when a read misses, the file NAME is searched
// under this directory (and across mounted pack entries) so refs survive
// assets being organized into subfolders. Set when a project opens.
void SetSearchRoot(const std::filesystem::path& assetsRoot);

// Reads a whole file. Pack-aware: if `path` lies under the mounted virtual
// root and the packs contain it, the bytes come from the pack; otherwise the
// read falls back to disk. Returns nullopt when the file exists nowhere.
std::optional<std::vector<u8>> ReadFile(const std::filesystem::path& path);

// True when `path` resolves either to a mounted pack entry or a disk file.
bool Exists(const std::filesystem::path& path);

} // namespace hbe::vfs
