// Collab/ProjectFetch.h - pulling a project from a peer, with no engine attached.
//
// WHY THIS IS ITS OWN THING. The download state machine first lived inside
// Editor/CollabSession, which includes Scene/ and therefore only exists inside an editor.
// But the person who most needs to download a project is the person who does not have one
// yet - and the tool they are holding at that moment is the HUB, which deliberately links
// no engine at all. Requiring them to install the engine, invent an empty project, open
// it, and only then join a session in order to fetch the real project defeats the point of
// the Hub being where you get everything.
//
// So the machine lives here: Core + STL + the collaboration client. Both the Hub and the
// editor drive the same one, rather than each growing its own copy that agrees with the
// other on the day it was written.
//
// IT DOES NOT OWN THE CONNECTION. The caller owns the transport and the CollabClient and
// feeds this the two messages it cares about. That is what lets the editor run a download
// over the SAME session it is already collaborating on, instead of opening a second one.
#pragma once

#include "Collab/CollabClient.h"
#include "Collab/ProjectSync.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::collab {

class ProjectFetch {
public:
    enum class Phase : u8 {
        Idle,
        AskingForList, // waiting for the host's manifest
        Downloading,
        Done,
        Failed,
    };
    static const char* PhaseName(Phase p);

    // `client` must outlive the fetch and must already be connected.
    void Attach(CollabClient* client) { client_ = client; }

    // Begins. `into` is created if absent; anything already there is kept and only what
    // differs is fetched, so an interrupted transfer resumes cheaply.
    bool Start(const std::filesystem::path& into);

    // The owner routes these in from its ClientCallbacks.
    void OnManifest(const MsgSyncManifest& m);
    void OnChunk(const MsgFileChunk& c);

    // NEITHER WORKING PHASE CAN EXIT ON ITS OWN. AskingForList and Downloading each wait
    // on a specific inbound message, and every refusal upstream is a bare return with no
    // reply - so a host that declines, drops, or simply never answers renders exactly like
    // one that is still working. Call this every frame with a real clock.
    void Tick(u64 nowMs);
    static constexpr u64 kStallMs = 30000;

    Phase State() const { return phase_; }
    usize FilesDone() const { return rx_.FilesDone(); }
    usize FilesTotal() const { return wanted_.size(); }
    u64 BytesReceived() const { return rx_.BytesReceived(); }
    const std::string& Error() const { return err_; }
    const std::filesystem::path& Root() const { return root_; }
    // The `.hbproj` inside what arrived, if there is one. Empty means the sender shared
    // files but not a project - worth saying, rather than opening nothing.
    std::filesystem::path ProjectFile() const;

private:
    void RequestNext();

    CollabClient* client_ = nullptr;
    Phase phase_ = Phase::Idle;
    SyncReceiver rx_;
    std::vector<SyncFile> wanted_;
    usize next_ = 0;
    std::filesystem::path root_;
    std::string err_;
    u64 lastProgressMs_ = 0; // 0 = no clock yet, so Tick cannot fire before the first
    u64 lastMark_ = 0;       // observed progress counter (bytes + files requested)
};

bool ProjectFetchSelfTest(); // folded into --test-projectsync

} // namespace hbe::collab
