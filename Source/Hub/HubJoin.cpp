// Hub/HubJoin.cpp
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Hub/HubJoin.h"

#include "Core/Platform.h"

#include "Collab/Signaling.h"

#include <chrono>
#include <cstdio>

namespace hbe::hub {

namespace fs = std::filesystem;

fs::path IdentityFileForHub() {
    // Deliberately duplicated from Editor/CollabSession's IdentityFile() rather than
    // shared: the Hub links no engine, and pulling in an editor header to agree on a path
    // would be the wrong dependency in the wrong direction. The PATH is the contract, and
    // --test-hub asserts the Hub and the editor produce the same fingerprint from it.
    // The PATH is the contract, and it now comes from one implementation - the editor's
    // copy of this derivation and this one used to be able to disagree about truncation
    // and about the fallback when LOCALAPPDATA is unset.
    return platform::UserDataDir() / "identity.hbkey";
}

JoinSession::JoinSession() = default;
JoinSession::~JoinSession() = default;

const char* JoinSession::StepName(Step s) {
    switch (s) {
    case Step::NeedInvitation: return "waiting for an invitation";
    case Step::Confirm: return "check who this is from";
    case Step::Connecting: return "connecting";
    case Step::Fetching: return "copying the project";
    case Step::Done: return "done";
    case Step::Failed: return "failed";
    }
    return "?";
}

bool JoinSession::LoadIdentity(const fs::path& keyFile) {
    if (id_.Valid()) return true;
    if (!id_.LoadOrCreate(keyFile)) {
        status_ = "Could not create this machine's key.";
        return false;
    }
    return true;
}

std::string JoinSession::Fingerprint() const {
    return id_.Valid() ? collab::Fingerprint(id_.Public()) : std::string("(no key yet)");
}

bool JoinSession::Paste(const std::string& invitationText) {
    collab::SessionBlob blob;
    if (!collab::DecodeSessionBlob(invitationText, blob) || blob.isAnswer) {
        status_ = "That does not look like an invitation. It should start with HBE-INVITE-1:";
        return false;
    }
    // Decoded, NOT connected. The fingerprint is shown first so the user can check it
    // against what they were told - the whole reason it rides in the blob.
    pending_ = invitationText;
    hostKey_ = blob.claimedKey;
    step_ = Step::Confirm;
    status_ = "This invitation says it is from " + collab::Fingerprint(hostKey_);
    return true;
}

std::string JoinSession::HostFingerprint() const { return collab::Fingerprint(hostKey_); }

bool JoinSession::Confirm(const fs::path& into) {
    if (step_ != Step::Confirm) return false;
    if (into.empty()) {
        status_ = "Choose where to put the project first.";
        return false;
    }
    if (!id_.Valid()) {
        status_ = "This machine has no key yet.";
        return false;
    }
    into_ = into;
    link_ = std::make_unique<collab::WebRtcClientTransport>();
    const collab::PublicKey expected = hostKey_;
    // PINNED to the advertised key. If the invitation was altered in transit the far end
    // cannot produce that key's signature and the handshake fails - which is exactly what
    // showing the fingerprint first is for.
    link_->EnableSecurity(id_, [expected](const collab::PublicKey& k) { return k == expected; });
    if (!link_->BeginFromInvitation(pending_)) {
        status_ = "Could not use that invitation.";
        link_.reset();
        step_ = Step::Failed;
        return false;
    }
    // Callbacks BEFORE the client is constructed - CollabClient copies them, so wiring
    // afterwards leaves it with an empty set and nothing ever arrives.
    callbacks_.onManifest = [this](const collab::MsgSyncManifest& m) { fetch_.OnManifest(m); };
    callbacks_.onFileChunk = [this](const collab::MsgFileChunk& c) { fetch_.OnChunk(c); };
    client_ = std::make_unique<collab::CollabClient>(link_.get(), callbacks_);
    step_ = Step::Connecting;
    status_ = "Preparing a reply...";
    return true;
}

void JoinSession::Cancel() {
    if (link_) link_->Disconnect();
    client_.reset();
    link_.reset();
    pending_.clear();
    reply_.clear();
    asked_ = false;
    step_ = Step::NeedInvitation;
    status_.clear();
}

void JoinSession::Tick() {
    if (!link_) return;
    // A REAL CLOCK. This passed 0 every call, so CollabClient's heartbeat condition
    // (now - last >= 2000) was never true and the host expired this peer's leases after
    // ten seconds mid-transfer.
    using namespace std::chrono;
    const u64 now = static_cast<u64>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    if (client_) client_->Pump(now);
    link_->Poll();
    if (client_) client_->Pump(now);

    if (reply_.empty() && !link_->Reply().empty()) {
        reply_ = link_->Reply();
        status_ = "Reply ready - send it back to them.";
    }
    // CLOSED, NOT FAILED, is what a link that breaks AFTER opening reports. Testing
    // Failed alone left this function returning at the Connected() guard below forever,
    // with the status frozen on whatever it last said - a Hub that looks busy but is dead.
    const collab::LinkState st = link_->State();
    if (st == collab::LinkState::Failed || st == collab::LinkState::Closed) {
        const char* why = link_->Error();
        status_ = (why && *why)
                      ? std::string("Could not connect: ") + why
                      : std::string("The connection dropped. Ask them for a fresh "
                                    "invitation and try again.");
        step_ = Step::Failed;
        return;
    }
    if (!link_->Connected()) return;

    if (!asked_) {
        asked_ = true;
        // Announce ourselves once. Without a UserId the host ignores every request we
        // make, silently - including the one for its file list.
        client_->Hello(collab::Fingerprint(id_.Public()));
        fetch_.Attach(client_.get());
        if (!fetch_.Start(into_)) {
            status_ = "Could not start the copy.";
            step_ = Step::Failed;
            return;
        }
        step_ = Step::Fetching;
    }

    // A stalled fetch has to fail on its own, or every silent refusal upstream shows up
    // as a Hub that just sits there.
    fetch_.Tick(now);

    switch (fetch_.State()) {
    case collab::ProjectFetch::Phase::Done:
        step_ = Step::Done;
        // Error() IS SET ON THE DONE PATH TOO: "they have not shared" finishes
        // successfully with nothing to copy. Reading it only under Failed threw away the
        // one sentence naming the real cause, in favour of one claiming a copy happened.
        status_ = !fetch_.Error().empty()
                      ? fetch_.Error()
                      : (fetch_.ProjectFile().empty()
                             ? std::string("Copied, but there is no project file in it.")
                             : std::string("Copied " + std::to_string(fetch_.FilesTotal()) +
                                           " file(s)."));
        break;
    case collab::ProjectFetch::Phase::Failed:
        step_ = Step::Failed;
        status_ = fetch_.Error();
        break;
    default:
        status_ = "Copying... " + std::to_string(fetch_.FilesDone()) + "/" +
                  std::to_string(fetch_.FilesTotal());
        break;
    }
}

bool HubJoinSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("hubjoin FAIL: %s\n", what);
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_hubjoin_test";
    fs::remove_all(dir, ec);

    JoinSession j;
    check(j.State() == JoinSession::Step::NeedInvitation, "a new join waits for an invitation");
    check(!j.Paste("nonsense"), "arbitrary text must be refused");
    check(!j.Confirm(dir / "landing"),
          "confirming before an invitation has been read must be refused");

    check(j.LoadIdentity(dir / "hub.hbkey"), "the Hub should create a keypair");
    const std::string fp = j.Fingerprint();
    check(fp.size() > 4 && fp != "(no key yet)", "it should have a readable fingerprint");
    // THE SAME KEY the editor would use from the same file - one person, one identity,
    // whichever tool is open.
    {
        collab::Identity same;
        check(same.LoadOrCreate(dir / "hub.hbkey"), "the key should reload");
        check(collab::Fingerprint(same.Public()) == fp,
              "the Hub and the editor must share ONE identity - a host allowlists a "
              "fingerprint, and it has to be the same one whichever tool they open");
    }

    // A REPLY pasted where an invitation belongs is a common mix-up that otherwise
    // produces a connection that hangs with no diagnosis.
    collab::SessionBlob reply;
    reply.isAnswer = true;
    reply.sdp = "v=0\r\n";
    check(!j.Paste(collab::EncodeSessionBlob(reply)),
          "a REPLY pasted where an invitation belongs must be refused");

    // A well-formed invitation is accepted and STOPS to show who it is from.
    collab::SessionBlob inv;
    inv.isAnswer = false;
    inv.claimedKey = collab::PublicKey{};
    inv.claimedKey[0] = 0xAB;
    inv.sdp = "v=0\r\no=- 1 0 IN IP4 127.0.0.1\r\n";
    check(j.Paste(collab::EncodeSessionBlob(inv)), "a well-formed invitation is accepted");
    check(j.State() == JoinSession::Step::Confirm,
          "it must STOP and ask, not connect to whatever it was handed");
    check(j.HostFingerprint() == collab::Fingerprint(inv.claimedKey),
          "the fingerprint shown must be the one in the invitation");
    check(!j.Confirm(""), "confirming with nowhere to put it must be refused");

    j.Cancel();
    check(j.State() == JoinSession::Step::NeedInvitation, "cancelling returns to the start");

    fs::remove_all(dir, ec);
    if (fails == 0)
        std::printf("hubjoin: the Hub holds the SAME identity as the editor, refuses junk "
                    "and a swapped reply, and stops to show who an invitation is from "
                    "before connecting to anything\n");
    return fails == 0;
}

} // namespace hbe::hub
