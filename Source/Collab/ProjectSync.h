// Collab/ProjectSync.h - handing a whole project to someone who has nothing.
//
// THE PROBLEM. Everything else in the collaboration layer assumes both peers already
// hold the same project and are exchanging CHANGES to it. That assumption has to be
// established somehow, and "email them a zip first" is not a feature - it is the step
// where the two copies silently diverge before anyone has edited anything.
//
// So: a peer with an empty folder connects, receives a manifest of what the host has,
// asks for the files it is missing, and ends up with a working copy. Same link, same
// identity, same allowlist - a file transfer is not a second trust boundary.
//
// WHAT IS DELIBERATELY NOT SENT. A project directory contains far more than the project:
// build output, an asset cache, importer intermediates, the collaborator's own key-based
// allowlist. Sending those is not merely wasteful - `Build/` alone can dwarf the actual
// content, and shipping one machine's `Library/` cache to another is how you transplant a
// stale index that then has to be diagnosed. Exclusion is a PURE, TESTED function
// (ShouldSync) rather than a filter buried in the walk, because it is the part that will
// need changing and the part whose mistakes are silent.
//
// SAFETY. Every path in a received manifest is attacker-controlled data - the peer is
// authenticated, but authenticated is not the same as trusted with your filesystem, and a
// single "../" in a manifest entry writes outside the project. This does not invent a
// guard for that: it reuses hub::SafeJoin, which already exists for zip-slip in the
// updater, is individually tested, and refuses absolute paths, "..", drive-relative
// paths, NUL bytes and Windows device names.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::collab {

struct SyncFile {
    std::string path;     // relative to the project root, forward slashes
    u64 size = 0;
    std::string sha256;   // lowercase hex; what makes "do I already have this?" answerable
};

struct ProjectManifest {
    std::vector<SyncFile> files;
    u64 TotalBytes() const;
};

// Whether a project-relative path is part of the PROJECT rather than of one machine's
// build of it.
//
// Pure and total so it can be tested exhaustively: this is the function that decides what
// a colleague receives, and a mistake here is either a 40 GB transfer or a missing asset
// that only shows up as a pink texture on someone else's machine.
bool ShouldSync(const std::string& relPath);

// Walks `root` and hashes what it finds. `outSkipped` counts what ShouldSync rejected,
// so the UI can say "1,200 files, build output excluded" rather than leaving the author
// wondering whether it is really sending everything.
//
// Returns false only on an unreadable root - an individual unreadable file is skipped and
// counted, because one locked file must not make a project unshareable.
bool BuildManifest(const std::filesystem::path& root, ProjectManifest& out,
                   usize* outSkipped = nullptr, usize* outUnreadable = nullptr);

// What the receiver still needs: everything in `theirs` whose path is absent from `mine`
// or whose hash differs. Also pure, so "why is it re-sending that?" has an answer.
ProjectManifest Missing(const ProjectManifest& theirs, const ProjectManifest& mine);

// Receives files into a STAGING directory and only then moves them into place.
//
// Staging is not tidiness. A transfer that writes directly into the project and then
// fails halfway has left a half-updated project that looks like a working one; the author
// finds out when the game will not load. Everything lands under `staging`, is verified
// against its hash, and is moved into the project as a last step.
class SyncReceiver {
public:
    // `root` is the project directory (created if absent); `staging` must be on the same
    // volume, or the final move becomes a copy and stops being atomic per file.
    bool Begin(const std::filesystem::path& root, const std::filesystem::path& staging,
               const ProjectManifest& expected);

    // Appends bytes for `relPath`. Refuses a path that is not in the manifest, a path
    // that escapes the root, and a file that grows beyond its declared size. Returns
    // false on any of those; the caller must abandon the transfer rather than continue,
    // because a peer sending unexpected paths is not a peer having a bad day.
    bool Write(const std::string& relPath, const u8* data, usize n);

    // Closes `relPath` and verifies its hash. A mismatch fails: the bytes are wrong, and
    // keeping them because they arrived is how a corrupt asset gets committed.
    bool Finish(const std::string& relPath);

    // Every expected file received and verified.
    bool Complete() const;
    usize FilesDone() const { return done_.size(); }
    u64 BytesReceived() const { return received_; }
    const std::string& Error() const { return err_; }

    // Moves everything staged into the project. Only call once Complete().
    bool Commit();
    // Deletes the staging directory. Safe at any point.
    void Abandon();

private:
    const SyncFile* Expect(const std::string& relPath) const;

    std::filesystem::path root_, staging_;
    ProjectManifest expected_;
    std::vector<std::string> done_;
    std::string open_;      // the file currently being written
    u64 openBytes_ = 0;
    u64 received_ = 0;
    std::string err_;
};

bool ProjectSyncSelfTest(); // --test-projectsync

} // namespace hbe::collab
