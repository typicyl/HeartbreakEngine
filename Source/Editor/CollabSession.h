// Editor/CollabSession.h - the editor's entire view of collaboration.
//
// WHY THIS EXISTS RATHER THAN LIVING IN Editor.cpp. Everything underneath - identity,
// the secure channel, ICE, the server, the client, the journal - is already built and
// tested headlessly. What was missing was a front door. Putting that state machine
// inline in an 18,000-line file would make it untestable and would tangle a network
// lifecycle with frame drawing; keeping it here means Editor.cpp only ever DRAWS this
// object and calls Tick() on it, and --test-collabsession can drive the whole flow with
// no window open.
//
// THE FLOW IT MODELS, end to end:
//
//   Host                                  Guest
//   ----                                  -----
//   Start hosting                         (needs an invitation)
//   Invite -> HBE-INVITE-1:...  --------> paste it
//                                         sees the host's FINGERPRINT and confirms
//                               <-------- HBE-REPLY-1:...
//   paste the reply
//   ...link forms, both prove their keys...
//   a guest not on the list is REFUSED, and shows up as "someone tried to join"
//   host adds their fingerprint; guest reconnects
//
// TWO THINGS IT REFUSES TO PAPER OVER:
//
//   * A guest whose key is not on the allowlist is refused, and the attempt is SURFACED
//     rather than silently dropped. A host who cannot see who tried has no way to invite
//     anyone, and would reasonably conclude the feature is broken.
//   * Sealing a commit needs the scene to have a docId. Minting one silently on save was
//     tried and reverted - it made saves non-deterministic, which --test-tagtable caught.
//     So enabling history on a scene is an explicit action that changes the file, once.
#pragma once

#include "Collab/CollabClient.h"
#include "Collab/CollabServer.h"
#include "Collab/Identity.h"
#include "Collab/Journal.h"
#include "Collab/ProjectSync.h"
#include "Collab/WebRtcTransport.h"
#include "Scene/SceneJournal.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace hbe::editor {

enum class SessionRole : u8 {
    Offline,     // no session; everything still works, which is the point
    Hosting,     // we hold the project state
    Confirming,  // an invitation is decoded and waiting for the user to trust the host
    Joining,     // connecting to someone else's session
    Joined,
};
const char* SessionRoleName(SessionRole r);

// Where a peer's own keypair lives: %LOCALAPPDATA%/HeartbreakEngine/identity.hbkey.
//
// Per USER, and deliberately not inside the engine install: the updater replaces the
// install directory wholesale, and losing the keypair would drop this machine out of
// every colleague's allowlist and orphan every commit it had ever signed.
std::filesystem::path IdentityFile();

// Where a project's allowlist lives. Per PROJECT, because it is that project's access
// control - a colleague invited to one is not thereby invited to all of them.
std::filesystem::path AllowlistFile(const std::filesystem::path& projectRoot);

// Where a scene's history lives: alongside the scene, so copying a project copies it.
std::filesystem::path JournalFile(const std::filesystem::path& scenePath);

class CollabSession {
public:
    CollabSession();
    ~CollabSession();
    CollabSession(const CollabSession&) = delete;
    CollabSession& operator=(const CollabSession&) = delete;

    // --- identity ---------------------------------------------------------
    // Loads the keypair, generating it on first run. Everything else refuses to work
    // until this succeeds, rather than falling back to something anonymous.
    // `file` empty = IdentityFile(), the real per-user location. It is a parameter only
    // so the self-test can drive two independent peers in one process; production code
    // must never pass one, or two projects would end up with two identities.
    bool EnsureIdentity(const std::filesystem::path& file = {});
    bool HasIdentity() const;
    // Overrides the ICE servers. The self-test passes an EMPTY set so it connects over
    // host candidates alone and needs no internet.
    void SetIceConfig(const collab::IceConfig& cfg) { ice_ = cfg; }
    std::string MyFingerprint() const;

    // --- people (the host's allowlist) ------------------------------------
    void SetProjectRoot(const std::filesystem::path& root);
    const std::filesystem::path& ProjectRoot() const { return projectRoot_; }
    const std::vector<collab::Allowlist::Entry>& People() const;
    // BY VALUE - see Allowlist::Add. The caller's key is a reference into the very
    // knock list this call removes the entry from.
    bool AllowPerson(collab::PublicKey k, const std::string& label);
    bool RemovePerson(collab::PublicKey k);

    // Someone who connected, proved a key, and was turned away because that key is not
    // on the list. Surfaced so the host can add them - this IS the invite flow.
    struct Knock {
        collab::PublicKey key{};
        std::string fingerprint;
    };
    const std::vector<Knock>& Knocks() const { return knocks_; }
    void ClearKnocks() { knocks_.clear(); }

    // --- hosting ----------------------------------------------------------
    bool StartHosting();
    // Begins gathering for one guest. The invitation appears a moment later.
    bool CreateInvitation();
    const std::string& Invitation() const { return invitation_; }
    collab::LinkState InvitationState() const;
    bool AcceptReply(const std::string& replyText);
    usize PeerCount() const;

    // --- joining ----------------------------------------------------------
    // Decodes an invitation WITHOUT connecting, so the user can be shown who it claims
    // to be from before anything is trusted. Role becomes Confirming.
    bool PrepareJoin(const std::string& invitationText);
    std::string PendingHostFingerprint() const;
    // Connects, pinned to the key the invitation advertised. If that was a lie the
    // handshake fails - which is the point of showing the fingerprint first.
    bool ConfirmJoin();
    void CancelJoin();
    const std::string& Reply() const { return reply_; }

    void Leave();

    // --- per frame --------------------------------------------------------
    void Tick(u64 nowMs);
    SessionRole Role() const { return role_; }
    bool Live() const { return role_ == SessionRole::Hosting || role_ == SessionRole::Joined; }
    const std::string& Status() const { return status_; }

    // --- real time --------------------------------------------------------
    //
    // Call once per frame with the entity the author currently has selected.
    //
    // LOCKS SCOPE THE WORK, and that is what makes this affordable. Detecting "what
    // changed" has no hook to hang on - the editor writes components in place through
    // 435 widget sites and entt's on_update never fires - so the only general answer is
    // to diff. Diffing the WHOLE scene every frame would cost O(entities x components)
    // JSON serializations; diffing the ONE entity the author is holding costs the same
    // whether the scene has ten objects or ten thousand.
    //
    // It also matches what the protocol already enforces: only the lock owner may write.
    // Selecting an object takes its lock; moving on releases it.
    void LiveSync(Scene& s, entt::entity selected, u64 nowMs);

    // Who holds this entity, for the "held by" badge. 0 = nobody.
    collab::UserId LockOwnerOf(u64 guid) const;
    // Whether the author may edit this entity right now. True when offline - a session
    // that is not running must never make the editor read-only.
    bool CanEdit(u64 guid) const;
    // Changes received from other people and applied to the scene this frame.
    usize AppliedThisFrame() const { return appliedThisFrame_; }

    // --- getting the whole project ----------------------------------------
    //
    // The step that lets someone start from an EMPTY FOLDER. Everything else in this
    // layer assumes both peers already hold the same project and are exchanging changes
    // to it; that assumption has to be established somehow, and "email them a zip first"
    // is the step where the two copies silently diverge before anyone has edited
    // anything.
    enum class DownloadPhase : u8 { Idle, AskingForList, Downloading, Done, Failed };
    static const char* DownloadPhaseName(DownloadPhase p);

    // HOST: offer the current project. Walks and hashes it once - not per frame.
    bool ShareProject();
    usize SharedFileCount() const { return shared_.files.size(); }

    // GUEST: pull everything the host has that we do not, into `into`.
    bool StartDownload(const std::filesystem::path& into);
    DownloadPhase Download() const { return phase_; }
    usize DownloadFilesDone() const { return rx_.FilesDone(); }
    usize DownloadFilesTotal() const { return wanted_.size(); }
    u64 DownloadBytes() const { return rx_.BytesReceived(); }
    const std::string& DownloadError() const { return downloadErr_; }
    const std::filesystem::path& DownloadRoot() const { return downloadRoot_; }

    // --- history ----------------------------------------------------------
    // Remembers what the scene looked like, so the next save can diff against it. Call
    // when a DOCUMENT is opened - a scene load or New Scene.
    //
    // Explicitly NOT on undo/redo or Play->Stop restore, even though those also replace
    // the world. Re-baselining there would erase the not-yet-committed delta, and the
    // next save would seal a commit that omits the undone work entirely.
    //
    // `include` must be the SAME predicate the editor uses to decide which entities get
    // written to this file. See scene::JournalFilter.
    void NoteSceneOpened(const Scene& s, const std::filesystem::path& scenePath,
                         scene::JournalFilter include = {});
    bool TrackingScene() const { return !scenePath_.empty(); }
    const std::filesystem::path& TrackedPath() const { return scenePath_; }

    // The document now lives at a different file (Save As, or the first save of a scene
    // that had no path). The history file follows it, and the baseline is CLEARED so the
    // first commit written there describes the whole scene rather than starting the
    // story halfway through.
    void RetargetScene(const Scene& s, const std::filesystem::path& scenePath,
                       scene::JournalFilter include = {});

    // Whether this scene can have history at all (i.e. it has a docId).
    static bool SceneHasHistory(const Scene& s);
    // Mints docId + guidEpoch. The CALLER must then save the scene - this only changes
    // the in-memory scene, because a function that silently rewrites the user's file is
    // not something a menu item should do behind their back.
    static bool EnableHistory(Scene& s);

    // Diffs against the remembered snapshot, appends a commit, re-snapshots. Returns the
    // number of changes sealed; 0 means nothing changed (not a failure).
    usize SealCommit(const Scene& s, const std::string& message, std::string* outWhy);

    const collab::Journal& History() const { return journal_; }

    // --- review -----------------------------------------------------------
    // Compares someone else's history against ours and parks the verdict for the UI.
    bool BeginReview(const collab::Journal& theirs, const Scene& s);
    bool HasReview() const { return hasReview_; }
    const collab::MergePlan& ReviewPlan() const { return review_; }
    std::vector<scene::Resolution>& Decisions() { return decisions_; }
    bool AllDecided() const;
    // Applies the reviewed merge. Refuses unless every question is answered.
    usize ApplyReview(Scene& s, std::string* outWhy);
    void DismissReview();

    const collab::Identity& Me() const { return id_; }

private:
    void NoteKnock(const collab::PublicKey& k);
    bool PolicyForGuest(const collab::PublicKey& k);

    collab::Identity id_;
    collab::Allowlist allow_;
    collab::IceConfig ice_ = collab::IceConfig::Default();
    std::filesystem::path projectRoot_;
    SessionRole role_ = SessionRole::Offline;
    std::string status_ = "Not connected";

    std::unique_ptr<collab::WebRtcServerTransport> host_;
    std::unique_ptr<collab::CollabServer> server_;
    collab::ConnId invite_ = 0;
    std::string invitation_;

    std::unique_ptr<collab::WebRtcClientTransport> guest_;
    std::unique_ptr<collab::CollabClient> client_;
    collab::ClientCallbacks callbacks_;
    std::string reply_;
    std::string pendingInvitation_;
    collab::PublicKey pendingHostKey_{};

    std::vector<Knock> knocks_;

    std::filesystem::path scenePath_;
    scene::JournalFilter include_;
    scene::JournalSnapshot snapshot_;
    collab::Journal journal_;
    u64 docId_ = 0, epoch_ = 0;

    // --- live-sync state ---
    void QueueRemote(const collab::MsgDeltaApplied& d);
    void WireCallbacks();
    u64 lockedGuid_ = 0;
    u64 lastLockTryMs_ = 0;                              // the entity we hold, 0 = none
    std::map<std::string, std::string> liveBaseline_; // last broadcast value per component
    std::vector<collab::MsgDeltaApplied> remote_;     // arrived, not yet applied
    usize appliedThisFrame_ = 0;
    std::unordered_map<u64, collab::UserId> lockOwners_; // guid -> holder, for badges

    // --- project download ---
    void OnManifest(const collab::MsgSyncManifest& m);
    void OnFileChunk(const collab::MsgFileChunk& c);
    void RequestNextFile();
    DownloadPhase phase_ = DownloadPhase::Idle;
    collab::MsgSyncManifest shared_;             // host: what we offer
    collab::SyncReceiver rx_;                    // guest: where it lands
    std::vector<collab::SyncFile> wanted_;       // guest: still to fetch
    usize nextWanted_ = 0;
    std::filesystem::path downloadRoot_;
    std::string downloadErr_;

    bool hasReview_ = false;
    collab::MergePlan review_;
    std::vector<scene::Resolution> decisions_;
};

// --test-collabsession: drives the whole front-door flow headlessly - two sessions in
// one process, invitation and reply, a guest refused and then invited, and a save
// sealing a commit that the other side reviews and applies.
bool CollabSessionSelfTest();

} // namespace hbe::editor
