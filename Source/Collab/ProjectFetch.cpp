// Collab/ProjectFetch.cpp
#include "Collab/ProjectFetch.h"

#include <cstdio>

namespace hbe::collab {

namespace fs = std::filesystem;

const char* ProjectFetch::PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::AskingForList: return "asking what they have";
    case Phase::Downloading: return "copying files";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    }
    return "?";
}

bool ProjectFetch::Start(const fs::path& into) {
    if (!client_ || into.empty()) return false;
    root_ = into;
    err_.clear();
    wanted_.clear();
    next_ = 0;
    phase_ = Phase::AskingForList;
    client_->RequestProject();
    return true;
}

void ProjectFetch::OnManifest(const MsgSyncManifest& m) {
    if (phase_ != Phase::AskingForList) return;

    ProjectManifest theirs;
    theirs.files.reserve(m.files.size());
    for (const SyncEntry& e : m.files)
        theirs.files.push_back(SyncFile{e.path, e.size, e.sha256});

    // What is already here. An empty folder yields an empty manifest and we take
    // everything; a partial one takes only what differs, compared BY CONTENT rather than
    // by timestamp - which is what makes an interrupted transfer cheap to resume.
    ProjectManifest mine;
    BuildManifest(root_, mine, nullptr, nullptr);
    const ProjectManifest missing = Missing(theirs, mine);
    wanted_ = missing.files;
    next_ = 0;

    if (wanted_.empty()) {
        phase_ = Phase::Done;
        if (theirs.files.empty())
            err_ = "They have not shared their project yet. Ask them to open the "
                   "Collaborate panel and press Share the project files, then try again.";
        return;
    }
    // Staged BESIDE the destination. A transfer that dies halfway must not leave a
    // half-written project that looks like a working one.
    const fs::path staging = root_.parent_path() / (root_.filename().string() + ".incoming");
    if (!rx_.Begin(root_, staging, missing)) {
        phase_ = Phase::Failed;
        err_ = rx_.Error();
        return;
    }
    phase_ = Phase::Downloading;
    RequestNext();
}

void ProjectFetch::Tick(u64 nowMs) {
    if (phase_ != Phase::AskingForList && phase_ != Phase::Downloading) return;

    // PROGRESS IS INFERRED, not reported. Deriving it from the counters the message
    // handlers already move keeps OnManifest/OnChunk clock-free, so nothing has to thread
    // a timestamp down from two different owners with two different frame loops.
    const u64 mark = BytesReceived() + static_cast<u64>(next_) +
                     (phase_ == Phase::Downloading ? 1u : 0u);
    if (lastProgressMs_ == 0 || mark != lastMark_ || nowMs < lastProgressMs_) {
        lastMark_ = mark;
        lastProgressMs_ = nowMs;
        return;
    }
    if (nowMs - lastProgressMs_ < kStallMs) return;
    const bool asking = (phase_ == Phase::AskingForList);
    phase_ = Phase::Failed;
    err_ = asking ? std::string("No answer from them after 30 seconds. Ask them to check "
                                "that they are still hosting.")
                  : "They stopped sending. The copy stalled with " +
                        std::to_string(FilesDone()) + " of " + std::to_string(FilesTotal()) +
                        " file(s) done.";
    // Do not leave a half-written .incoming tree behind looking like a real project.
    rx_.Abandon();
}

void ProjectFetch::RequestNext() {
    if (next_ >= wanted_.size()) {
        if (!rx_.Commit()) {
            phase_ = Phase::Failed;
            err_ = rx_.Error();
            return;
        }
        phase_ = Phase::Done;
        return;
    }
    // ONE AT A TIME. The server serves one file per peer, so asking for several would
    // forget all but the last and the transfer would stall on a file nobody is sending.
    client_->RequestFile(wanted_[next_].path);
}

void ProjectFetch::OnChunk(const MsgFileChunk& c) {
    if (phase_ != Phase::Downloading || next_ >= wanted_.size()) return;
    if (c.path != wanted_[next_].path) return; // a stale chunk from a cancelled ask

    if (!c.data.empty() && !rx_.Write(c.path, c.data.data(), c.data.size())) {
        phase_ = Phase::Failed;
        err_ = rx_.Error();
        return;
    }
    if (!c.last) return;
    if (!rx_.Finish(c.path)) {
        // A hash mismatch or a short file. Stopping is right: continuing would commit a
        // project containing one asset that is quietly wrong.
        phase_ = Phase::Failed;
        err_ = rx_.Error();
        return;
    }
    ++next_;
    RequestNext();
}

fs::path ProjectFetch::ProjectFile() const {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) return {};
    for (const auto& de : fs::directory_iterator(root_, ec)) {
        if (ec) break;
        if (de.path().extension() == ".hbproj") return de.path();
    }
    return {};
}

bool ProjectFetchSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("projectfetch FAIL: %s\n", what);
    };
    // Without a client there is nothing to ask, and saying so beats sitting in a phase
    // that never advances.
    ProjectFetch f;
    check(!f.Start("C:/nowhere"), "a fetch with no connection must refuse to start");
    check(f.State() == ProjectFetch::Phase::Idle, "a refused start must not change phase");
    check(f.ProjectFile().empty(), "an empty fetch has no project file");
    check(std::string(ProjectFetch::PhaseName(ProjectFetch::Phase::Downloading)) !=
              std::string("?"),
          "every phase should have a name for the UI");
    if (fails == 0)
        std::printf("projectfetch: refuses to start without a connection and names every "
                    "phase\n");
    return fails == 0;
}

} // namespace hbe::collab
