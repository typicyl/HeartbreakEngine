// Hub/HubJoin.h - getting a project from a colleague, from the launcher, with nothing
// installed and nothing open.
//
// THE GAP THIS CLOSES. The Hub installs the engine and lists projects you already have.
// But the person the Hub exists for is the one who has NEITHER - and the flow before this
// was: install the engine, invent an empty project so the editor has something to open,
// open it, find the Collaborate panel, and only then download the real project. Every one
// of those steps is the launcher failing to be the place you get everything.
//
// So the Hub joins a session itself. It needs no engine to do it: Source/Collab/ was kept
// free of engine includes precisely so the client half could link somewhere like this.
//
// IT JOINS, IT DOES NOT HOST. No server, no locks, no journal - the Hub is not an editor
// and should not pretend to be one. It fetches files and then hands off to the editor.
#pragma once

#include "Collab/Identity.h"
#include "Collab/ProjectFetch.h"
#include "Collab/WebRtcTransport.h"

#include <filesystem>
#include <memory>
#include <string>

namespace hbe::hub {

class JoinSession {
public:
    JoinSession();
    ~JoinSession();
    JoinSession(const JoinSession&) = delete;
    JoinSession& operator=(const JoinSession&) = delete;

    enum class Step : u8 {
        NeedInvitation, // paste what they sent you
        Confirm,        // we know who it claims to be from; the user says yes
        Connecting,     // producing a reply, then linking up
        Fetching,
        Done,
        Failed,
    };
    static const char* StepName(Step s);

    // THE SAME KEYPAIR THE EDITOR USES. Deliberately: a person is one identity whichever
    // of their tools is running, so a fingerprint they read out from the Hub is the one a
    // host allowlists, and it keeps working when they open the editor.
    bool LoadIdentity(const std::filesystem::path& keyFile);
    std::string Fingerprint() const;

    // Decodes WITHOUT connecting, so the user can be shown who it claims to be from
    // before anything is trusted.
    bool Paste(const std::string& invitationText);
    std::string HostFingerprint() const;
    // Connects, pinned to the advertised key. `into` is where the project will land.
    bool Confirm(const std::filesystem::path& into);
    void Cancel();

    // The text to send back. Empty until it is ready.
    const std::string& Reply() const { return reply_; }

    void Tick();

    Step State() const { return step_; }
    const std::string& Status() const { return status_; }
    usize FilesDone() const { return fetch_.FilesDone(); }
    usize FilesTotal() const { return fetch_.FilesTotal(); }
    u64 Bytes() const { return fetch_.BytesReceived(); }
    // The `.hbproj` that arrived, for handing to the editor. Empty if none did.
    std::filesystem::path ProjectFile() const { return fetch_.ProjectFile(); }

private:
    collab::Identity id_;
    std::unique_ptr<collab::WebRtcClientTransport> link_;
    std::unique_ptr<collab::CollabClient> client_;
    collab::ClientCallbacks callbacks_;
    collab::ProjectFetch fetch_;
    collab::PublicKey hostKey_{};
    std::string pending_, reply_, status_;
    std::filesystem::path into_;
    Step step_ = Step::NeedInvitation;
    bool asked_ = false;
};

// %LOCALAPPDATA%/HeartbreakEngine/identity.hbkey - THE SAME FILE the editor uses.
// One person is one identity whichever of their tools is open, so the fingerprint they
// read out from the Hub is the one that keeps working in the editor.
std::filesystem::path IdentityFileForHub();

bool HubJoinSelfTest(); // folded into --test-hub

} // namespace hbe::hub
