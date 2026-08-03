// Hub/Updater.h - check, download, stage, swap, relaunch.
//
// THE SELF-REPLACEMENT PROBLEM. A running .exe cannot be overwritten on Windows, so an
// updater cannot simply unpack over its own install. What Windows DOES allow is
// RENAMING a running executable. So the swap is:
//
//   1. download to  <install>/_update/download.zip
//   2. extract to   <install>/_update/staged/         (nothing live is touched yet)
//   3. rename       <install>/bin -> <install>/_update/backup_<version>
//   4. rename       <install>/_update/staged/bin -> <install>/bin
//   5. on ANY failure in 3-4, rename the backup back
//
// Steps 3 and 4 are directory renames on the same volume: near-atomic, and reversible.
// Everything expensive and failure-prone (network, disk, decompression, the integrity
// check) happens in step 1-2 where nothing live has been touched, so the common failure
// modes cannot leave a half-installed engine.
//
// The Hub itself lives OUTSIDE the payload it swaps, which is what makes this legal at
// all: it is not renaming the directory it is running from.
#pragma once

#include "Hub/UpdateCheck.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace hbe::hub {

enum class UpdateState : u8 {
    Idle,
    Checking,
    UpToDate,
    Available,    // a newer version exists; waiting for the user to say yes
    Downloading,
    Verifying,
    Installing,
    Done,         // installed; relaunch to use it
    Failed,
};
const char* UpdateStateName(UpdateState s);

struct UpdateProgress {
    UpdateState state = UpdateState::Idle;
    u64 bytesDone = 0;
    u64 bytesTotal = 0; // 0 = the server did not say
    std::string message;   // human-facing; safe to show verbatim
    Version localVersion;
    Version remoteVersion;
    std::string releaseUrl;
};

// Where an install lives and where its scratch space goes.
struct UpdatePaths {
    std::filesystem::path installRoot; // the directory holding bin/, shaders/, ...
    std::filesystem::path Work() const { return installRoot / "_update"; }
    std::filesystem::path Download() const { return Work() / "download.zip"; }
    std::filesystem::path Staged() const { return Work() / "staged"; }
    std::filesystem::path BackupFor(const Version& v) const {
        return Work() / ("backup_" + v.ToString());
    }
};

class Updater {
public:
    // `manifestUrl` must be https (UrlIsSafe). Anything else fails immediately and
    // loudly rather than being silently downgraded.
    Updater(std::string manifestUrl, UpdatePaths paths);

    // The version ALREADY INSTALLED at paths.installRoot, which is what Check compares
    // against. Set it from the install's own stamp, not from the Hub's compile-time
    // constant: the Hub is a separate download now and its number says nothing about
    // what engine sits in that folder. nullopt = nothing installed (or an unreadable
    // stamp), which makes any published version an INSTALL rather than an update.
    void SetInstalledVersion(std::optional<Version> v);
    void SetInstallRoot(const std::filesystem::path& root) { paths_.installRoot = root; }
    const UpdatePaths& Paths() const { return paths_; }
    // True when Check found something and there is no engine to replace - the wording,
    // the button and the backup step all differ between installing and updating.
    bool IsFreshInstall() const { return !installed_.has_value(); }

    // Fetches and parses the manifest. Blocking; call it off the UI's critical path or
    // accept a brief stall. Sets state to UpToDate / Available / Failed.
    void Check();

    // Downloads, verifies, extracts and swaps. Only legal from Available.
    //
    // `confirm` is called ONCE, after the manifest is known and BEFORE anything is
    // downloaded, with the version and URL. Returning false aborts. This exists so an
    // update can never be applied without a human having seen what it is - the Hub
    // wires it to a dialog; a headless caller can pass a lambda that returns false.
    void Apply(const std::function<bool(const UpdateProgress&)>& confirm);

    const UpdateProgress& Progress() const { return progress_; }
    // True when an install completed and the engine on disk is now newer than the one
    // this process is running. The Hub uses it to offer a relaunch.
    bool NeedsRelaunch() const { return progress_.state == UpdateState::Done; }

    // Removes staging and old backups. Safe to call at boot; never touches the live
    // install. Keeps the MOST RECENT backup so a bad update stays revertible.
    void CleanWorkspace();
    // Puts the most recent backup back. Returns false when there is none.
    bool Rollback(std::string& outError);

private:
    void Fail(std::string why);

    std::string manifestUrl_;
    UpdatePaths paths_;
    UpdateProgress progress_;
    UpdateManifest manifest_;
    bool haveManifest_ = false;
    std::optional<Version> installed_; // nullopt = nothing installed yet
};

// --- HTTPS, via WinHTTP (ships with Windows; no third-party dependency) -------
//
// Both refuse anything that is not https and leave certificate validation ON. There is
// deliberately no "insecure" flag: an updater with cert checks disabled is worse than
// no updater, and an option that exists will eventually be set by someone debugging.

// Downloads a small resource into memory. `maxBytes` bounds the response so a hostile
// or broken server cannot exhaust memory.
bool HttpGetString(const std::string& url, std::string& out, usize maxBytes,
                   std::string& outError);

// Streams a resource to a file, reporting progress. `onProgress` may return false to
// cancel, which deletes the partial file rather than leaving it to be mistaken for a
// complete download.
bool HttpDownloadFile(const std::string& url, const std::filesystem::path& dest,
                      const std::function<bool(u64 done, u64 total)>& onProgress,
                      u64 maxBytes, std::string& outError);

// Lowercase hex SHA-256 of a file, via the Windows CNG API. Empty on failure.
std::string Sha256File(const std::filesystem::path& file);

bool UpdaterSelfTest(); // part of --test-hub

} // namespace hbe::hub
