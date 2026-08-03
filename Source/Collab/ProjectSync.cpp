// Collab/ProjectSync.cpp
#include "Collab/ProjectSync.h"

#include "Collab/ProjectFetch.h"
#include "Hub/Updater.h"    // Sha256File - already used by the installer
#include "Hub/ZipArchive.h" // SafeJoin - the zip-slip guard, reused rather than reinvented

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unordered_map>

namespace hbe::collab {

namespace fs = std::filesystem;

namespace {

// Directories that are one machine's BUILD of the project, not the project.
//
// Matched as a whole path component, never as a substring: a folder legitimately called
// "Libraries" or an asset called "objects.uaf" must not vanish because "Library" and
// "obj" appear inside them. That substring bug is silent - the file is simply never sent,
// and shows up as a missing asset on the other machine.
const char* const kSkipDirs[] = {
    "Build",   // shipped output
    "Library", // import cache, per machine
    "obj",     // MSVC intermediates
    "out",     // CMake build tree, if someone nests one
    ".git",
    ".vs",
    "Saves", // one player's save games; not project content
};

bool IsSkippedDir(const std::string& part) {
    for (const char* d : kSkipDirs)
        if (part == d) return true;
    // "Assets.backup-preslots", "Scenes.backup-preguid" - migration backups the engine
    // leaves behind. They are one machine's safety copy of content that is ALSO being
    // sent in its current form, so shipping them doubles the transfer for nothing.
    return part.find(".backup-") != std::string::npos;
}

bool IsSkippedFile(const std::string& name) {
    // The host's own access-control list. Sending it would hand a guest the roster and,
    // worse, let a guest's copy overwrite the host's on a later sync.
    if (name == "collab_authorized.txt") return true;
    if (name == "identity.hbkey") return true; // never; it is a private key
    return false;
}

std::string ToRel(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const fs::path rel = fs::relative(p, root, ec);
    if (ec) return {};
    std::string s = rel.generic_string();
    return s;
}

} // namespace

u64 ProjectManifest::TotalBytes() const {
    u64 n = 0;
    for (const SyncFile& f : files) n += f.size;
    return n;
}

bool ShouldSync(const std::string& relPath) {
    if (relPath.empty()) return false;
    // A path that escapes, or is absolute, is not a project-relative path at all. Refusing
    // here as well as in SafeJoin means a bad entry cannot even reach the manifest.
    if (relPath.find("..") != std::string::npos) return false;
    if (relPath.find(':') != std::string::npos) return false;
    if (relPath[0] == '/' || relPath[0] == '\\') return false;

    usize start = 0;
    std::string last;
    while (start <= relPath.size()) {
        const usize slash = relPath.find('/', start);
        const std::string part =
            relPath.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() && slash != std::string::npos) return false; // "a//b"
        if (slash == std::string::npos) {
            last = part;
            break;
        }
        if (IsSkippedDir(part)) return false;
        start = slash + 1;
    }
    return !IsSkippedFile(last);
}

bool BuildManifest(const fs::path& root, ProjectManifest& out, usize* outSkipped,
                   usize* outUnreadable) {
    out.files.clear();
    usize skipped = 0, unreadable = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ++unreadable;
            ec.clear();
            continue;
        }
        const fs::directory_entry& e = *it;
        const std::string rel = ToRel(root, e.path());
        if (rel.empty()) continue;

        if (e.is_directory(ec)) {
            // Prune whole trees rather than walking them and discarding every entry - a
            // Build/ directory can hold tens of thousands of files.
            if (IsSkippedDir(e.path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec)) continue; // symlinks, devices: not project content
        if (!ShouldSync(rel)) {
            ++skipped;
            continue;
        }
        const u64 size = static_cast<u64>(e.file_size(ec));
        if (ec) {
            ++unreadable;
            ec.clear();
            continue;
        }
        const std::string hash = hub::Sha256File(e.path());
        if (hash.empty()) {
            // One locked or unreadable file must not make the project unshareable.
            ++unreadable;
            continue;
        }
        out.files.push_back(SyncFile{rel, size, hash});
    }
    // Deterministic order, so two hosts with the same content produce the same manifest
    // and a diff of two manifests is readable.
    std::sort(out.files.begin(), out.files.end(),
              [](const SyncFile& a, const SyncFile& b) { return a.path < b.path; });
    if (outSkipped) *outSkipped = skipped;
    if (outUnreadable) *outUnreadable = unreadable;
    return true;
}

ProjectManifest Missing(const ProjectManifest& theirs, const ProjectManifest& mine) {
    std::unordered_map<std::string, const SyncFile*> have;
    have.reserve(mine.files.size());
    for (const SyncFile& f : mine.files) have[f.path] = &f;

    ProjectManifest out;
    for (const SyncFile& f : theirs.files) {
        const auto it = have.find(f.path);
        // Compared BY HASH, not by size or timestamp. Two files of the same length are
        // routinely different, and a timestamp says when a machine touched a file rather
        // than what is in it.
        if (it == have.end() || it->second->sha256 != f.sha256) out.files.push_back(f);
    }
    return out;
}

// --- receiving ----------------------------------------------------------------

const SyncFile* SyncReceiver::Expect(const std::string& relPath) const {
    for (const SyncFile& f : expected_.files)
        if (f.path == relPath) return &f;
    return nullptr;
}

bool SyncReceiver::Begin(const fs::path& root, const fs::path& staging,
                         const ProjectManifest& expected) {
    root_ = root;
    staging_ = staging;
    expected_ = expected;
    done_.clear();
    open_.clear();
    openBytes_ = 0;
    received_ = 0;
    err_.clear();

    std::error_code ec;
    fs::remove_all(staging_, ec); // a previous abandoned transfer must not be inherited
    fs::create_directories(staging_, ec);
    if (ec) {
        err_ = "could not create the staging folder";
        return false;
    }
    fs::create_directories(root_, ec);
    return true;
}

bool SyncReceiver::Write(const std::string& relPath, const u8* data, usize n) {
    const SyncFile* want = Expect(relPath);
    if (!want) {
        // A peer sending a file nobody asked for is not a peer having a bad day.
        err_ = "the sender offered a file that is not in the manifest: " + relPath;
        return false;
    }
    const fs::path dest = hub::SafeJoin(staging_, relPath);
    if (dest.empty()) {
        err_ = "the sender offered a path that escapes the project: " + relPath;
        return false;
    }
    if (open_ != relPath) {
        open_ = relPath;
        openBytes_ = 0;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            err_ = "could not create " + relPath;
            return false;
        }
    }
    if (openBytes_ + n > want->size) {
        // Without this a sender can fill the disk by never ending a file.
        err_ = "the sender sent more bytes than it declared for " + relPath;
        return false;
    }
    std::ofstream out(dest, std::ios::binary | std::ios::app);
    if (!out) {
        err_ = "could not write " + relPath;
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!out.good()) {
        err_ = "could not write " + relPath;
        return false;
    }
    openBytes_ += n;
    received_ += n;
    return true;
}

bool SyncReceiver::Finish(const std::string& relPath) {
    const SyncFile* want = Expect(relPath);
    if (!want) {
        err_ = "finished a file that is not in the manifest: " + relPath;
        return false;
    }
    const fs::path dest = hub::SafeJoin(staging_, relPath);
    if (dest.empty()) {
        err_ = "the sender offered a path that escapes the project: " + relPath;
        return false;
    }
    std::error_code ec;
    // A zero-byte file never reaches Write(), so create it here rather than failing it.
    if (!fs::exists(dest, ec)) {
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            err_ = "could not create " + relPath;
            return false;
        }
    }
    if (static_cast<u64>(fs::file_size(dest, ec)) != want->size || ec) {
        err_ = "wrong size for " + relPath;
        return false;
    }
    if (hub::Sha256File(dest) != want->sha256) {
        // Keeping bytes because they arrived is how a corrupt asset gets committed and
        // then blamed on the exporter.
        err_ = "the contents of " + relPath + " do not match what was advertised";
        return false;
    }
    if (std::find(done_.begin(), done_.end(), relPath) == done_.end()) done_.push_back(relPath);
    open_.clear();
    openBytes_ = 0;
    return true;
}

bool SyncReceiver::Complete() const { return done_.size() == expected_.files.size(); }

bool SyncReceiver::Commit() {
    if (!Complete()) {
        err_ = "not every file arrived";
        return false;
    }
    std::error_code ec;
    for (const SyncFile& f : expected_.files) {
        const fs::path from = hub::SafeJoin(staging_, f.path);
        const fs::path to = hub::SafeJoin(root_, f.path);
        if (from.empty() || to.empty()) {
            err_ = "unsafe path at commit: " + f.path;
            return false;
        }
        fs::create_directories(to.parent_path(), ec);
        ec.clear();
        // Rename over the destination. rename() refuses an existing target on Windows, so
        // the existing file is removed first; both are on the same volume by construction
        // (the caller stages inside the project's parent), which keeps this a move.
        fs::remove(to, ec);
        ec.clear();
        fs::rename(from, to, ec);
        if (ec) {
            // Falling back to a copy keeps a transfer working across volumes; it is less
            // atomic, and saying so beats silently doing something different.
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                err_ = "could not move " + f.path + " into the project";
                return false;
            }
        }
    }
    Abandon();
    return true;
}

void SyncReceiver::Abandon() {
    std::error_code ec;
    if (!staging_.empty()) fs::remove_all(staging_, ec);
}

// --- self-test ----------------------------------------------------------------

namespace {

int g_psFails = 0;
void Check(bool c, const char* what) {
    if (c) return;
    ++g_psFails;
    std::printf("projectsync FAIL: %s\n", what);
}

void WriteFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

bool ProjectSyncSelfTest() {
    g_psFails = 0;
    if (!ProjectFetchSelfTest()) ++g_psFails;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_projectsync_test";
    fs::remove_all(dir, ec);

    // --- the exclusion policy, exhaustively -----------------------------------
    Check(ShouldSync("Assets/Scenes/Game.hbscene"), "an asset should sync");
    Check(ShouldSync("Game.hbproj"), "the project file should sync");
    // These three are the whole reason the skip list matches WHOLE path components. Each
    // name genuinely CONTAINS a skipped name as a substring, and each is an ordinary
    // thing to call a folder in a game project. A substring match drops them silently -
    // the file is simply never sent, and it surfaces as a missing asset on the other
    // machine with nothing pointing back here.
    Check(ShouldSync("Assets/Buildings/wall.uaf"),
          "a folder called 'Buildings' CONTAINS 'Build' but is not build output");
    Check(ShouldSync("Assets/objects/crate.uaf"),
          "a folder called 'objects' CONTAINS 'obj' but is not an intermediates folder");
    Check(ShouldSync("Assets/outfits/coat.uaf"),
          "a folder called 'outfits' CONTAINS 'out' but is not a build tree");
    Check(ShouldSync("Assets/Textures/objects.png"),
          "a FILE whose name contains 'obj' must still sync");
    Check(!ShouldSync("Build/Game.exe"), "build output must not sync");
    Check(!ShouldSync("Library/cache.bin"), "the import cache must not sync");
    Check(!ShouldSync("obj/x.obj"), "intermediates must not sync");
    Check(!ShouldSync("Assets.backup-preslots/old.hbscene"),
          "a migration backup must not sync - the same content is already being sent in "
          "its current form");
    Check(!ShouldSync("collab_authorized.txt"),
          "the host's own access list must NEVER be sent - it would hand over the roster "
          "and let a guest's copy overwrite it later");
    Check(!ShouldSync("identity.hbkey"), "a private key must never be sent");
    Check(!ShouldSync("../outside.txt"), "an escaping path must be refused");
    Check(!ShouldSync("/etc/passwd"), "an absolute path must be refused");
    Check(!ShouldSync("C:/Windows/x.dll"), "a drive-qualified path must be refused");
    Check(!ShouldSync(""), "an empty path must be refused");

    // --- a real project tree --------------------------------------------------
    const fs::path host = dir / "host";
    WriteFile(host / "Game.hbproj", "{}");
    WriteFile(host / "Assets" / "Scenes" / "Game.hbscene", "scene bytes");
    WriteFile(host / "Assets" / "Textures" / "wall.png", "png bytes");
    WriteFile(host / "Build" / "Game.exe", "should not travel");
    WriteFile(host / "Library" / "cache.bin", "should not travel");
    WriteFile(host / "collab_authorized.txt", "secret roster");
    WriteFile(host / "Assets" / "empty.txt", "");

    ProjectManifest m;
    usize skipped = 0, unreadable = 0;
    Check(BuildManifest(host, m, &skipped, &unreadable), "the manifest should build");
    Check(m.files.size() == 4,
          "exactly the four project files belong in the manifest");
    for (const SyncFile& f : m.files) {
        Check(f.path.rfind("Build/", 0) != 0, "build output leaked into the manifest");
        Check(f.path.rfind("Library/", 0) != 0, "the cache leaked into the manifest");
        Check(f.path != "collab_authorized.txt", "the access list leaked into the manifest");
    }
    Check(skipped >= 1, "the excluded files should be counted, not silently dropped");
    // Deterministic order - two hosts with the same content must agree.
    ProjectManifest m2;
    BuildManifest(host, m2, nullptr, nullptr);
    Check(m.files.size() == m2.files.size(), "the manifest should be stable");
    for (usize i = 0; i < m.files.size() && i < m2.files.size(); ++i)
        Check(m.files[i].path == m2.files[i].path && m.files[i].sha256 == m2.files[i].sha256,
              "the manifest must be deterministic");

    // --- what a peer still needs ----------------------------------------------
    {
        ProjectManifest mine;
        Check(Missing(m, mine).files.size() == m.files.size(),
              "a peer with NOTHING needs everything");
        mine = m;
        Check(Missing(m, mine).files.empty(), "a peer with everything needs nothing");
        mine.files[1].sha256 = "deadbeef";
        Check(Missing(m, mine).files.size() == 1,
              "only the file whose CONTENTS differ should be re-sent");
    }

    // --- receiving ------------------------------------------------------------
    const fs::path guest = dir / "guest";
    const fs::path staging = dir / "staging";
    {
        SyncReceiver rx;
        Check(rx.Begin(guest, staging, m), "the receiver should start");
        for (const SyncFile& f : m.files) {
            std::ifstream in(host / f.path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            // In chunks, as the wire delivers them.
            for (usize off = 0; off < bytes.size(); off += 4) {
                const usize n = std::min<usize>(4, bytes.size() - off);
                Check(rx.Write(f.path, reinterpret_cast<const u8*>(bytes.data() + off), n),
                      "a chunk should be accepted");
            }
            Check(rx.Finish(f.path), "a completed file should verify");
        }
        Check(rx.Complete(), "every file should have arrived");
        Check(rx.Commit(), "the transfer should commit");
        Check(fs::exists(guest / "Assets" / "Scenes" / "Game.hbscene"),
              "the scene should have landed");
        Check(fs::exists(guest / "Assets" / "empty.txt"),
              "a ZERO-BYTE file must land too - it never reaches Write(), so a receiver "
              "that only creates files there loses it silently");
        Check(!fs::exists(guest / "Build"), "build output must not have landed");
        Check(!fs::exists(staging), "the staging folder should be gone after committing");
    }

    // --- every way a sender must be refused -----------------------------------
    {
        SyncReceiver rx;
        rx.Begin(dir / "g2", dir / "s2", m);
        const u8 b[4] = {1, 2, 3, 4};
        Check(!rx.Write("../escape.txt", b, 4),
              "a path escaping the project must be REFUSED, not written");
        Check(!rx.Write("Assets/never-offered.bin", b, 4),
              "a file that is not in the manifest must be refused");
        // Overrun: declare a small file, send more.
        const SyncFile& small = m.files.front();
        std::vector<u8> flood(small.size + 64, 0x41);
        Check(!rx.Write(small.path, flood.data(), flood.size()),
              "a sender exceeding its declared size must be refused - without this it can "
              "fill the disk by never ending a file");
        rx.Abandon();
    }
    {
        // Right size, wrong bytes.
        SyncReceiver rx;
        rx.Begin(dir / "g3", dir / "s3", m);
        const SyncFile& f = m.files.front();
        const std::vector<u8> junk(static_cast<usize>(f.size), 0x5A);
        rx.Write(f.path, junk.data(), junk.size());
        Check(!rx.Finish(f.path),
              "contents that do not match the advertised hash must FAIL - keeping bytes "
              "because they arrived is how a corrupt asset gets committed");
        Check(!rx.Complete(), "a failed file must not count as done");
        Check(!rx.Commit(), "an incomplete transfer must not commit");
        Check(!fs::exists(dir / "g3" / f.path),
              "nothing from a failed transfer may reach the project");
        rx.Abandon();
    }

    fs::remove_all(dir, ec);
    if (g_psFails == 0) {
        std::printf("projectsync: build output, caches, migration backups and the host's "
                    "own access list stay out; the manifest is deterministic and content-"
                    "addressed so only genuinely different files re-send; a transfer "
                    "lands through staging with zero-byte files intact; and an escaping "
                    "path, an unoffered file, an oversized file and wrong contents are "
                    "each refused with nothing reaching the project\n");
    }
    return g_psFails == 0;
}

} // namespace hbe::collab
