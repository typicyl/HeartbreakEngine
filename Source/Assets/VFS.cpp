// Assets/VFS.cpp
#include "Assets/VFS.h"

#include "Assets/UAP.h"
#include "Core/Log.h"

#include <fstream>
#include <mutex>
#include <unordered_map>

namespace hbe::vfs {

namespace fs = std::filesystem;

namespace {

struct Mount {
    uap::PackSet packs;
    fs::path virtualRoot; // absolute; pack entries are relative to this
    bool active = false;

    fs::path searchRoot; // filename-fallback root (the project's Assets/)
    // Filename -> resolved path. Guarded: scene streaming stages assets on a
    // worker thread while the editor reads thumbnails on the main thread.
    std::mutex cacheMutex;
    std::unordered_map<std::string, fs::path> resolveCache;
};

Mount& TheMount() {
    static Mount m;
    return m;
}

// Filename fallback: refs written before assets were organized into folders
// (or by older importers) carry just a name; find that name anywhere in the
// mounted packs or under the search root. Cached per filename.
std::optional<std::vector<u8>> ReadByFilename(const fs::path& path) {
    Mount& m = TheMount();
    const std::string filename = path.filename().string();
    if (filename.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(m.cacheMutex);
    if (auto it = m.resolveCache.find(filename); it != m.resolveCache.end()) {
        if (it->second.empty()) return std::nullopt; // cached miss
        if (m.active) {
            if (auto bytes = m.packs.Read(it->second.generic_string())) return bytes;
        }
        std::ifstream in(it->second, std::ios::binary | std::ios::ate);
        if (in) {
            const std::streamsize size = in.tellg();
            in.seekg(0);
            std::vector<u8> bytes(static_cast<usize>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (in) return bytes;
        }
        return std::nullopt;
    }

    // Mounted packs: match any entry by trailing filename.
    if (m.active) {
        for (const uap::Entry& e : m.packs.Entries()) {
            if (fs::path(e.path).filename().string() == filename) {
                m.resolveCache[filename] = fs::path(e.path);
                HBE_WARN("VFS: resolved '{}' by filename to pack entry '{}'.",
                         filename, e.path);
                return m.packs.Read(e.path);
            }
        }
    }
    // Disk: recursive search under the search root.
    if (!m.searchRoot.empty()) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m.searchRoot, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file() || it->path().filename().string() != filename) {
                continue;
            }
            m.resolveCache[filename] = it->path();
            HBE_WARN("VFS: resolved '{}' by filename to '{}'.", filename,
                     it->path().string());
            std::ifstream in(it->path(), std::ios::binary | std::ios::ate);
            if (!in) break;
            const std::streamsize size = in.tellg();
            in.seekg(0);
            std::vector<u8> bytes(static_cast<usize>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (in) return bytes;
            break;
        }
    }
    m.resolveCache[filename] = fs::path(); // cache the miss
    return std::nullopt;
}

// Pack key for `path` when it lies under the mounted root (else nullopt).
std::optional<std::string> PackKey(const fs::path& path) {
    const Mount& m = TheMount();
    if (!m.active) return std::nullopt;
    std::error_code ec;
    const fs::path abs = path.is_absolute() ? path : fs::absolute(path, ec);
    const fs::path rel = fs::relative(abs, m.virtualRoot, ec);
    if (ec || rel.empty() || rel.native().starts_with(L"..")) return std::nullopt;
    return rel.generic_string();
}

std::optional<std::vector<u8>> ReadDiskFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<u8> bytes(static_cast<usize>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) return std::nullopt;
    return bytes;
}

} // namespace

bool MountPacks(const fs::path& packDir, const std::string& baseName,
                const fs::path& virtualRoot) {
    Mount& m = TheMount();
    m.active = false;
    if (!m.packs.Open(packDir, baseName)) {
        return false;
    }
    std::error_code ec;
    m.virtualRoot = fs::absolute(virtualRoot, ec);
    m.active = !ec;
    if (m.active) {
        HBE_INFO("VFS: mounted {} pack chunk(s) ({} assets) for '{}'.",
                 m.packs.PackCount(), m.packs.AssetCount(), m.virtualRoot.string());
    }
    return m.active;
}

void Unmount() {
    Mount& m = TheMount(); // member-wise reset: the mutex is not assignable
    m.packs = uap::PackSet{};
    m.virtualRoot.clear();
    m.active = false;
    m.searchRoot.clear();
    std::lock_guard<std::mutex> lock(m.cacheMutex);
    m.resolveCache.clear();
}

bool IsMounted() {
    return TheMount().active;
}

void SetSearchRoot(const fs::path& assetsRoot) {
    Mount& m = TheMount();
    std::error_code ec;
    m.searchRoot = fs::absolute(assetsRoot, ec);
    std::lock_guard<std::mutex> lock(m.cacheMutex);
    m.resolveCache.clear();
}

std::optional<std::vector<u8>> ReadFile(const fs::path& path) {
    if (const auto key = PackKey(path)) {
        if (auto bytes = TheMount().packs.Read(*key)) return bytes;
    }
    if (auto bytes = ReadDiskFile(path)) return bytes;
    return ReadByFilename(path); // moved/renamed-folder fallback
}

bool Exists(const fs::path& path) {
    if (const auto key = PackKey(path)) {
        if (TheMount().packs.Contains(*key)) return true;
    }
    std::error_code ec;
    return fs::exists(path, ec);
}

} // namespace hbe::vfs
