// Editor/CollabSession.cpp
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Editor/CollabSession.h"

#include "Scene/Scene.h"

#include <bcrypt.h>

#include "Scene/Components.h"
#include "Scene/SceneSerializer.h"
#include "Scene/EntityGuid.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace hbe::editor {

namespace fs = std::filesystem;

namespace {

u64 NowMsWall() {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

u64 RandomU64() {
    u64 v = 0;
    if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return 0;
    return v;
}

fs::path LocalAppData() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
}

} // namespace

const char* SessionRoleName(SessionRole r) {
    switch (r) {
    case SessionRole::Offline: return "offline";
    case SessionRole::Hosting: return "hosting";
    case SessionRole::Confirming: return "confirming";
    case SessionRole::Joining: return "joining";
    case SessionRole::Joined: return "joined";
    }
    return "?";
}

fs::path IdentityFile() { return LocalAppData() / "HeartbreakEngine" / "identity.hbkey"; }

fs::path AllowlistFile(const fs::path& projectRoot) {
    return projectRoot / "collab_authorized.txt";
}

fs::path JournalFile(const fs::path& scenePath) {
    fs::path p = scenePath;
    p += ".hbjournal";
    return p;
}

CollabSession::CollabSession() = default;
CollabSession::~CollabSession() = default;

// --- identity -----------------------------------------------------------------

bool CollabSession::EnsureIdentity(const fs::path& file) {
    if (id_.Valid()) return true;
    if (!id_.LoadOrCreate(file.empty() ? IdentityFile() : file)) {
        status_ = "Could not create this machine's key.";
        return false;
    }
    return true;
}

bool CollabSession::HasIdentity() const { return id_.Valid(); }

std::string CollabSession::MyFingerprint() const {
    return id_.Valid() ? collab::Fingerprint(id_.Public()) : std::string("(no key yet)");
}

// --- people -------------------------------------------------------------------

void CollabSession::SetProjectRoot(const fs::path& root) {
    if (projectRoot_ == root) return;
    projectRoot_ = root;
    allow_ = collab::Allowlist();
    if (!root.empty()) allow_.Load(AllowlistFile(root)); // absent = nobody yet, not an error
}

const std::vector<collab::Allowlist::Entry>& CollabSession::People() const {
    return allow_.Entries();
}

bool CollabSession::AllowPerson(collab::PublicKey k, const std::string& label) {
    if (projectRoot_.empty()) {
        status_ = "Open a project before inviting anyone.";
        return false;
    }
    allow_.Add(k, label);
    // Saved IMMEDIATELY. An allowlist that only persists on some later "save project"
    // would silently forget a colleague across a crash, and the symptom - "they could
    // join yesterday" - points nowhere near the cause.
    if (!allow_.Save(AllowlistFile(projectRoot_))) {
        status_ = "Could not write the people list.";
        return false;
    }
    // They are welcome now, so they are no longer knocking.
    for (usize i = knocks_.size(); i-- > 0;)
        if (knocks_[i].key == k) knocks_.erase(knocks_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

bool CollabSession::RemovePerson(collab::PublicKey k) {
    if (!allow_.Remove(k)) return false;
    if (projectRoot_.empty()) return false;
    return allow_.Save(AllowlistFile(projectRoot_));
}

void CollabSession::NoteKnock(const collab::PublicKey& k) {
    for (const Knock& s : knocks_)
        if (s.key == k) return; // one entry per person, however many times they retry
    knocks_.push_back(Knock{k, collab::Fingerprint(k)});
    status_ = "Someone tried to join: " + collab::Fingerprint(k);
}

bool CollabSession::PolicyForGuest(const collab::PublicKey& k) {
    if (allow_.Allows(k)) return true;
    // Refused - but REMEMBERED. A host who cannot see who tried has no way to invite
    // anybody, and would reasonably conclude the feature does not work.
    NoteKnock(k);
    return false;
}

// --- hosting ------------------------------------------------------------------

bool CollabSession::StartHosting() {
    if (!EnsureIdentity()) return false;
    if (role_ != SessionRole::Offline) return false;
    host_ = std::make_unique<collab::WebRtcServerTransport>();
    host_->SetIceConfig(ice_);
    host_->EnableSecurity(id_, [this](const collab::PublicKey& k) { return PolicyForGuest(k); });
    server_ = std::make_unique<collab::CollabServer>(host_.get());
    // The host is the authority, so nothing broadcasts a guest's edit back to it.
    server_->onApplied = [this](const collab::MsgDeltaApplied& d) { QueueRemote(d); };
    role_ = SessionRole::Hosting;
    status_ = "Hosting. Create an invitation to add someone.";
    return true;
}

bool CollabSession::CreateInvitation() {
    if (role_ != SessionRole::Hosting || !host_) return false;
    invitation_.clear();
    invite_ = host_->CreateInvitation();
    if (invite_ == 0) {
        status_ = "Could not start a connection.";
        return false;
    }
    status_ = "Preparing an invitation...";
    return true;
}

collab::LinkState CollabSession::InvitationState() const {
    if (!host_ || invite_ == 0) return collab::LinkState::Closed;
    return host_->StateOf(invite_);
}

bool CollabSession::AcceptReply(const std::string& replyText) {
    if (role_ != SessionRole::Hosting || !host_ || invite_ == 0) return false;
    if (!host_->AcceptReply(invite_, replyText)) {
        status_ = "That does not look like a reply. Make sure it starts with HBE-REPLY-1:";
        return false;
    }
    status_ = "Connecting...";
    return true;
}

usize CollabSession::PeerCount() const { return server_ ? server_->PeerCount() : 0; }

// --- joining ------------------------------------------------------------------

bool CollabSession::PrepareJoin(const std::string& invitationText) {
    if (!EnsureIdentity()) return false;
    if (role_ != SessionRole::Offline) return false;
    collab::SessionBlob blob;
    if (!collab::DecodeSessionBlob(invitationText, blob) || blob.isAnswer) {
        status_ = "That does not look like an invitation. It should start with HBE-INVITE-1:";
        return false;
    }
    // Decoded but NOT connected. The user sees who it claims to be from first - the whole
    // reason the fingerprint is in the blob.
    pendingInvitation_ = invitationText;
    pendingHostKey_ = blob.claimedKey;
    role_ = SessionRole::Confirming;
    status_ = "This invitation claims to be from " + collab::Fingerprint(pendingHostKey_);
    return true;
}

std::string CollabSession::PendingHostFingerprint() const {
    return collab::Fingerprint(pendingHostKey_);
}

bool CollabSession::ConfirmJoin() {
    if (role_ != SessionRole::Confirming) return false;
    guest_ = std::make_unique<collab::WebRtcClientTransport>();
    guest_->SetIceConfig(ice_);
    const collab::PublicKey expected = pendingHostKey_;
    // PINNED to the key the invitation advertised. If the blob was tampered with in
    // transit, the far end cannot produce that key's signature and the handshake fails -
    // which is exactly what showing the fingerprint first is for.
    guest_->EnableSecurity(id_, [expected](const collab::PublicKey& k) { return k == expected; });
    if (!guest_->BeginFromInvitation(pendingInvitation_)) {
        status_ = "Could not use that invitation.";
        guest_.reset();
        role_ = SessionRole::Offline;
        return false;
    }
    // BEFORE constructing the client: CollabClient COPIES the callbacks at
    // construction, so wiring them afterwards would leave it with the empty set and
    // every remote edit would arrive nowhere.
    WireCallbacks();
    client_ = std::make_unique<collab::CollabClient>(guest_.get(), callbacks_);
    role_ = SessionRole::Joining;
    status_ = "Preparing a reply...";
    return true;
}

void CollabSession::CancelJoin() {
    if (role_ != SessionRole::Confirming) return;
    pendingInvitation_.clear();
    pendingHostKey_ = collab::PublicKey{};
    role_ = SessionRole::Offline;
    status_ = "Not connected";
}

void CollabSession::Leave() {
    if (guest_) guest_->Disconnect();
    client_.reset();
    guest_.reset();
    server_.reset();
    host_.reset();
    invite_ = 0;
    invitation_.clear();
    reply_.clear();
    pendingInvitation_.clear();
    role_ = SessionRole::Offline;
    status_ = "Not connected";
    // Knocks are kept deliberately: "someone tried to join while you were hosting" is
    // still the thing the host needs to act on after they stop.
}

// --- per frame ----------------------------------------------------------------

void CollabSession::Tick(u64 nowMs) {
    if (host_) {
        host_->Poll();
        if (server_) server_->Tick(nowMs);
        host_->Poll();
        if (invite_ != 0 && invitation_.empty() &&
            host_->StateOf(invite_) == collab::LinkState::WaitingForPeer) {
            invitation_ = host_->Invitation(invite_);
            status_ = "Invitation ready - send it to them.";
        }
        if (invite_ != 0 && host_->StateOf(invite_) == collab::LinkState::Open &&
            !invitation_.empty()) {
            status_ = "Connected. " + std::to_string(PeerCount()) + " here.";
        }
    }
    if (guest_) {
        if (client_) client_->Pump(nowMs);
        guest_->Poll();
        if (client_) client_->Pump(nowMs);
        if (reply_.empty() && !guest_->Reply().empty()) {
            reply_ = guest_->Reply();
            status_ = "Reply ready - send it back to them.";
        }
        if (role_ == SessionRole::Joining && guest_->Connected()) {
            role_ = SessionRole::Joined;
            status_ = "Connected.";
            // Announce ourselves ONCE, on the transition. Without this the server never
            // assigns a UserId, so every lock request is silently ignored and nothing
            // this peer does can ever be accepted.
            if (client_) {
                client_->Hello(collab::Fingerprint(id_.Public()));
                client_->JoinDocument(docId_);
            }
        } else if (guest_->State() == collab::LinkState::Failed) {
            const char* why = guest_->Error();
            status_ = (why && *why) ? std::string("Could not connect: ") + why
                                    : std::string("Could not connect.");
            role_ = SessionRole::Offline;
            client_.reset();
            guest_.reset();
        }
    }
}

// --- getting the whole project --------------------------------------------------

const char* CollabSession::DownloadPhaseName(DownloadPhase p) {
    switch (p) {
    case DownloadPhase::Idle: return "idle";
    case DownloadPhase::AskingForList: return "asking what they have";
    case DownloadPhase::Downloading: return "copying files";
    case DownloadPhase::Done: return "done";
    case DownloadPhase::Failed: return "failed";
    }
    return "?";
}

bool CollabSession::ShareProject() {
    if (!server_ || projectRoot_.empty()) return false;
    collab::ProjectManifest man;
    usize skipped = 0, unreadable = 0;
    // Walked and HASHED ONCE, here, not per frame: this reads every file in the project.
    if (!collab::BuildManifest(projectRoot_, man, &skipped, &unreadable)) {
        status_ = "Could not read the project folder.";
        return false;
    }
    shared_.files.clear();
    shared_.files.reserve(man.files.size());
    for (const collab::SyncFile& f : man.files)
        shared_.files.push_back(collab::SyncEntry{f.path, f.size, f.sha256});
    server_->ShareProject(projectRoot_.generic_string(), shared_);
    status_ = "Sharing " + std::to_string(shared_.files.size()) + " file(s)" +
              (unreadable ? " (" + std::to_string(unreadable) + " could not be read)" : "");
    return true;
}

bool CollabSession::StartDownload(const fs::path& into) {
    if (role_ != SessionRole::Joined || !client_) return false;
    if (into.empty()) return false;
    downloadRoot_ = into;
    downloadErr_.clear();
    wanted_.clear();
    nextWanted_ = 0;
    phase_ = DownloadPhase::AskingForList;
    status_ = "Asking what they have...";
    client_->RequestProject();
    return true;
}

void CollabSession::OnManifest(const collab::MsgSyncManifest& m) {
    if (phase_ != DownloadPhase::AskingForList) return;
    collab::ProjectManifest theirs;
    theirs.files.reserve(m.files.size());
    for (const collab::SyncEntry& e : m.files)
        theirs.files.push_back(collab::SyncFile{e.path, e.size, e.sha256});

    // What we ALREADY have. An empty folder yields an empty manifest and we take
    // everything; a folder that is already most of the way there takes only what differs,
    // compared BY CONTENT rather than by timestamp.
    collab::ProjectManifest mine;
    collab::BuildManifest(downloadRoot_, mine, nullptr, nullptr);
    const collab::ProjectManifest missing = collab::Missing(theirs, mine);
    wanted_ = missing.files;
    nextWanted_ = 0;

    if (wanted_.empty()) {
        phase_ = DownloadPhase::Done;
        status_ = theirs.files.empty() ? "They are not sharing a project."
                                       : "Already up to date.";
        return;
    }
    // Staged BESIDE the project, not inside it: a transfer that dies halfway must not
    // leave a half-updated project that looks like a working one.
    const fs::path staging = downloadRoot_.parent_path() /
                             (downloadRoot_.filename().string() + ".incoming");
    if (!rx_.Begin(downloadRoot_, staging, missing)) {
        phase_ = DownloadPhase::Failed;
        downloadErr_ = rx_.Error();
        return;
    }
    phase_ = DownloadPhase::Downloading;
    RequestNextFile();
}

void CollabSession::RequestNextFile() {
    if (nextWanted_ >= wanted_.size()) {
        if (!rx_.Commit()) {
            phase_ = DownloadPhase::Failed;
            downloadErr_ = rx_.Error();
            return;
        }
        phase_ = DownloadPhase::Done;
        status_ = "Copied " + std::to_string(wanted_.size()) + " file(s).";
        return;
    }
    // ONE AT A TIME. The server serves one file per peer, so asking for several would
    // simply forget all but the last and the transfer would stall on a file nobody is
    // sending.
    client_->RequestFile(wanted_[nextWanted_].path);
}

void CollabSession::OnFileChunk(const collab::MsgFileChunk& c) {
    if (phase_ != DownloadPhase::Downloading) return;
    if (nextWanted_ >= wanted_.size()) return;
    if (c.path != wanted_[nextWanted_].path) return; // a stale chunk from a cancelled ask

    if (!c.data.empty() && !rx_.Write(c.path, c.data.data(), c.data.size())) {
        phase_ = DownloadPhase::Failed;
        downloadErr_ = rx_.Error();
        return;
    }
    if (!c.last) return;
    if (!rx_.Finish(c.path)) {
        // A hash mismatch or a short file. Stopping is right: continuing would commit a
        // project containing one asset that is quietly wrong.
        phase_ = DownloadPhase::Failed;
        downloadErr_ = rx_.Error();
        return;
    }
    ++nextWanted_;
    status_ = "Copying... " + std::to_string(nextWanted_) + "/" +
              std::to_string(wanted_.size());
    RequestNextFile();
}

// --- real time ------------------------------------------------------------------

void CollabSession::QueueRemote(const collab::MsgDeltaApplied& d) {
    // Queued, never applied here. These callbacks fire from inside CollabClient::Pump /
    // CollabServer::Tick, and touching the registry there would mutate the scene from
    // the middle of a network drain - the single-threaded ordering the whole layer
    // depends on. LiveSync drains this at ONE defined point in the frame.
    remote_.push_back(d);
}

void CollabSession::WireCallbacks() {
    callbacks_.onDelta = [this](const collab::MsgDeltaApplied& d) { QueueRemote(d); };
    callbacks_.onManifest = [this](const collab::MsgSyncManifest& m) { OnManifest(m); };
    callbacks_.onFileChunk = [this](const collab::MsgFileChunk& c) { OnFileChunk(c); };
    callbacks_.onLock = [this](const collab::MsgLockGrant& g) {
        // owner 0 = released or expired. Kept so the inspector can say who is holding
        // something rather than just refusing to let you type into it.
        if (g.view.owner == 0) lockOwners_.erase(g.view.key.guid);
        else lockOwners_[g.view.key.guid] = g.view.owner;
    };
}

collab::UserId CollabSession::LockOwnerOf(u64 guid) const {
    if (server_) {
        collab::EntityKey k;
        k.doc = docId_;
        k.guid = guid;
        return server_->LockOf(k).owner;
    }
    const auto it = lockOwners_.find(guid);
    return it == lockOwners_.end() ? 0 : it->second;
}

bool CollabSession::CanEdit(u64 guid) const {
    // OFFLINE MEANS YES. A collaboration feature that makes the editor read-only when
    // nothing is running would be worse than not having it.
    if (!Live()) return true;
    const collab::UserId owner = LockOwnerOf(guid);
    if (owner == 0) return true; // nobody holds it; taking it is the next frame's job
    if (server_) return owner == server_->LocalUser();
    return client_ && owner == client_->User();
}

void CollabSession::LiveSync(Scene& s, entt::entity selected, u64 nowMs) {
    appliedThisFrame_ = 0;
    if (!Live() || docId_ == 0) {
        remote_.clear();
        return;
    }
    entt::registry& reg = s.Registry();

    // --- 1. apply what other people did --------------------------------------
    for (const collab::MsgDeltaApplied& d : remote_) {
        // DOC FILTER. Every broadcast reaches every peer regardless of which document
        // they have open, so without this one scene's edits land in another.
        if (d.key.doc != docId_) continue;
        entt::entity target = entt::null;
        for (const entt::entity e : reg.view<Guid>())
            if (reg.get<Guid>(e).value == d.key.guid) { target = e; break; }
        if (target == entt::null) continue; // an object this copy does not have yet
        if (scene::ApplyComponentJson(s, target, d.componentKey, d.json) ==
                scene::DeltaApply::Applied ||
            d.json.empty())
            ++appliedThisFrame_;
        // If it landed on the entity WE are holding, fold it into the baseline too, or
        // the next diff would see their change as ours and send it straight back.
        if (d.key.guid == lockedGuid_) {
            if (d.json.empty()) liveBaseline_.erase(d.componentKey);
            else liveBaseline_[d.componentKey] = d.json;
        }
    }
    remote_.clear();

    // --- 2. follow the selection with a lock ---------------------------------
    u64 wantGuid = 0;
    if (selected != entt::null && reg.valid(selected) && reg.all_of<Guid>(selected))
        wantGuid = reg.get<Guid>(selected).value;

    if (wantGuid != lockedGuid_) {
        if (lockedGuid_ != 0) {
            collab::EntityKey old;
            old.doc = docId_;
            old.guid = lockedGuid_;
            if (server_) server_->LocalUnlock(old);
            else if (client_) client_->ReleaseLock(old);
        }
        lockedGuid_ = wantGuid;
        liveBaseline_.clear();
        if (lockedGuid_ != 0) {
            collab::EntityKey k;
            k.doc = docId_;
            k.guid = lockedGuid_;
            if (server_) server_->LocalLock(k, nowMs);
            else if (client_) client_->RequestLock(k);
            // Seed the baseline from the CURRENT value so selecting an object does not
            // immediately broadcast everything about it as though it had just changed.
            for (const std::string& key : scene::DeltaComponentKeys()) {
                std::string j;
                if (scene::ComponentToJson(s, selected, key, j)) liveBaseline_[key] = std::move(j);
            }
        }
    }
    if (lockedGuid_ == 0) return;

    // --- 3. broadcast what changed on the entity we hold ---------------------
    collab::EntityKey k;
    k.doc = docId_;
    k.guid = lockedGuid_;
    bool held = server_ ? server_->LocalHoldsLock(k)
                        : (client_ && client_->LockOwner(k) == client_->User());
    if (!held) {
        // KEEP ASKING. Requesting once when the selection changed was not enough: if the
        // object was held by someone else at that instant - which is exactly what happens
        // when two people are looking at the same thing - the request is refused and, with
        // no retry, that object stays permanently uneditable for the rest of the session
        // even after the other person walks away from it.
        //
        // Throttled rather than every frame: TryAcquire treats a repeat from the same
        // owner as a renewal, so this is cheap, but a 120 Hz request stream for an object
        // somebody else is dragging is pure noise on the wire.
        if (nowMs - lastLockTryMs_ >= 500) {
            lastLockTryMs_ = nowMs;
            if (server_) held = server_->LocalLock(k, nowMs);
            else if (client_) client_->RequestLock(k);
        }
        if (!held) return;
        // Just acquired it: re-seed from the CURRENT scene so we do not immediately
        // broadcast the other person's last value back at them as though it were ours.
        liveBaseline_.clear();
        for (const std::string& key : scene::DeltaComponentKeys()) {
            std::string j;
            if (scene::ComponentToJson(s, selected, key, j)) liveBaseline_[key] = std::move(j);
        }
        return;
    }

    for (const std::string& key : scene::DeltaComponentKeys()) {
        std::string now;
        const bool present = scene::ComponentToJson(s, selected, key, now);
        const auto was = liveBaseline_.find(key);
        if (present) {
            if (was != liveBaseline_.end() && was->second == now) continue; // unchanged
            liveBaseline_[key] = now;
            if (server_) server_->LocalDelta(k, key, now);
            else client_->SendDelta(k, key, now);
        } else if (was != liveBaseline_.end()) {
            // The component was removed. Empty json is the removal on the wire.
            liveBaseline_.erase(key);
            if (server_) server_->LocalDelta(k, key, std::string());
            else client_->SendDelta(k, key, std::string());
        }
    }
}

// --- history ------------------------------------------------------------------

bool CollabSession::SceneHasHistory(const Scene& s) { return s.Environment().docId != 0; }

bool CollabSession::EnableHistory(Scene& s) {
    if (s.Environment().docId != 0) return false; // already has one; re-minting would orphan its history
    const u64 doc = RandomU64(), epoch = RandomU64();
    if (doc == 0 || epoch == 0) return false;
    s.Environment().docId = doc;
    s.Environment().guidEpoch = epoch;
    return true;
}

void CollabSession::NoteSceneOpened(const Scene& s, const fs::path& scenePath,
                                    scene::JournalFilter include) {
    scenePath_ = scenePath;
    include_ = std::move(include);
    docId_ = s.Environment().docId;
    epoch_ = s.Environment().guidEpoch;
    snapshot_ = scene::SnapshotForJournal(s, include_);
    journal_.Clear();
    if (!scenePath.empty()) {
        bool truncated = false;
        journal_.Load(JournalFile(scenePath), &truncated);
        if (truncated)
            status_ = "The last entry in this scene's history was incomplete and was "
                      "discarded.";
    }
}

void CollabSession::RetargetScene(const Scene& s, const fs::path& scenePath,
                                  scene::JournalFilter include) {
    scenePath_ = scenePath;
    if (include) include_ = std::move(include);
    docId_ = s.Environment().docId;
    epoch_ = s.Environment().guidEpoch;
    journal_.Clear();
    bool truncated = false;
    if (!scenePath.empty()) journal_.Load(JournalFile(scenePath), &truncated);
    // CLEARED, not carried over. The history at the new path knows nothing about this
    // scene, so the next commit must describe all of it; diffing against the old file's
    // baseline would write a commit that only makes sense next to a journal that is not
    // there.
    snapshot_.clear();
}

usize CollabSession::SealCommit(const Scene& s, const std::string& message,
                                std::string* outWhy) {
    const auto fail = [&](const char* why) -> usize {
        if (outWhy) *outWhy = why;
        return 0;
    };
    if (!id_.Valid()) return fail("this machine has no key yet");
    if (scenePath_.empty()) return fail("no scene is being tracked");
    if (docId_ == 0)
        return fail("this scene has no history yet - turn it on from the Collaborate panel");
    // A scene that was saved somewhere else, or a docId that changed under us, would make
    // the diff meaningless.
    if (s.Environment().docId != docId_ || s.Environment().guidEpoch != epoch_)
        return fail("this scene's identity changed since it was opened; reopen it");

    const scene::JournalSnapshot now = scene::SnapshotForJournal(s, include_);
    std::vector<collab::Change> changes = scene::DiffSnapshots(snapshot_, now);
    if (changes.empty()) {
        // Not a failure. Saving an unchanged scene should not manufacture an empty commit
        // that every future merge then has to reason about.
        if (outWhy) outWhy->clear();
        return 0;
    }

    const collab::PeerId me = collab::PeerIdFromKey(id_.Public());
    u64 n = 0;
    for (const collab::Commit& c : journal_.Commits())
        if (c.id.peer == me && c.id.n >= n) n = c.id.n + 1;

    collab::Commit c;
    c.id = collab::CommitId{me, n};
    c.parent = journal_.Head();
    c.doc = docId_;
    c.guidEpoch = epoch_;
    c.timestampMs = NowMsWall();
    c.author = collab::Fingerprint(id_.Public());
    c.message = message.empty() ? std::string("saved scene") : message;
    c.changes = std::move(changes);

    if (!journal_.Append(JournalFile(scenePath_), c))
        return fail("could not write this scene's history file");

    // Re-baseline ONLY after the append succeeded. Moving it earlier would mean a failed
    // write silently discarded the changes from the next diff too, losing them for good.
    snapshot_ = now;
    if (outWhy) outWhy->clear();
    return c.changes.size();
}

// --- review -------------------------------------------------------------------

bool CollabSession::BeginReview(const collab::Journal& theirs, const Scene& s) {
    if (s.Environment().docId == 0) return false;
    review_ = collab::PlanMerge(journal_, theirs, s.Environment().docId, s.Environment().guidEpoch);
    decisions_.assign(review_.conflicts.size(), scene::Resolution::Undecided);
    hasReview_ = true;
    return true;
}

bool CollabSession::AllDecided() const {
    for (const scene::Resolution r : decisions_)
        if (r == scene::Resolution::Undecided) return false;
    return true;
}

usize CollabSession::ApplyReview(Scene& s, std::string* outWhy) {
    if (!hasReview_) {
        if (outWhy) *outWhy = "nothing to review";
        return 0;
    }
    usize missing = 0;
    usize applied = 0;
    if (review_.verdict == collab::MergeVerdict::NeedsReview) {
        bool refused = false;
        applied = scene::ApplyReviewedMerge(s, review_, decisions_, &missing, &refused);
        if (refused) {
            if (outWhy) *outWhy = "every change still needs a decision";
            return 0;
        }
    } else {
        applied = scene::ApplyMergePlan(s, review_, &missing);
        if (applied == 0 && !review_.toApply.empty()) {
            if (outWhy) *outWhy = review_.explanation;
            return 0;
        }
    }
    if (outWhy) {
        outWhy->clear();
        if (missing > 0) {
            // Never silent: a change naming an object this scene does not have means the
            // two sides disagree about what exists, and calling that a clean merge is how
            // someone loses work without being told.
            *outWhy = std::to_string(missing) +
                      " change(s) named objects this scene does not have and were skipped";
        }
    }
    DismissReview();
    return applied;
}

// --- self-test ------------------------------------------------------------------

namespace {

int g_csFails = 0;
void Check(bool c, const char* what) {
    if (c) return;
    ++g_csFails;
    std::printf("collabsession FAIL: %s\n", what);
}

// Named to avoid the Win32 WriteFile that windows.h drags in - which is not a compile
// error at the call site, it is a silent overload resolution to a totally different API.
void WriteTestFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The same rule the editor saves the active document with.
bool ActiveOnly(const entt::registry& reg, entt::entity e) {
    return !reg.all_of<SceneSource>(e);
}

template <typename Fn>
bool WaitUntil(Fn cond, int maxMs, const std::function<void()>& pump) {
    for (int waited = 0; waited < maxMs; waited += 10) {
        pump();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pump();
    return cond();
}

} // namespace

bool CollabSessionSelfTest() {
    g_csFails = 0;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_collabsession_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ================================================================
    // PART 1 - HISTORY. What Ctrl+S records, and what it must NOT.
    // ================================================================
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity mine = s.CreateEntity("Mine");
        reg.emplace_or_replace<Guid>(mine, Guid{1001});
        const entt::entity also = s.CreateEntity("Also");
        reg.emplace_or_replace<Guid>(also, Guid{1002});
        // An entity belonging to an ADDITIVELY STREAMED scene - a different document,
        // written to a different file by the same Ctrl+S.
        const entt::entity streamed = s.CreateEntity("FromAnotherFile");
        reg.emplace_or_replace<Guid>(streamed, Guid{2001});
        reg.emplace_or_replace<SceneSource>(streamed, SceneSource{"Other.hbscene"});

        const fs::path scenePath = dir / "Level.hbscene";
        CollabSession cs;
        Check(cs.EnsureIdentity(dir / "a.hbkey"), "an identity should be created");

        std::string why;
        cs.NoteSceneOpened(s, scenePath, &ActiveOnly);
        Check(cs.SealCommit(s, "x", &why) == 0 && !why.empty(),
              "a scene with NO history must refuse to record, with a reason");

        Check(CollabSession::EnableHistory(s), "history should turn on");
        Check(!CollabSession::EnableHistory(s),
              "turning history on TWICE must refuse - re-minting the id would orphan "
              "everything already recorded against the old one");
        cs.NoteSceneOpened(s, scenePath, &ActiveOnly);

        Check(cs.SealCommit(s, "no changes", &why) == 0 && why.empty(),
              "saving an UNCHANGED scene must record nothing and not be an error - an "
              "empty change set is something every future merge would have to reason "
              "about");

        reg.get<Name>(mine).value = "Renamed";
        reg.get<Name>(streamed).value = "AlsoRenamed"; // a DIFFERENT document's edit
        const usize n = cs.SealCommit(s, "saved scene", &why);
        Check(why.empty(), "a real change should record cleanly");
        Check(cs.History().Commits().size() == 1, "one change set should be recorded");
        Check(n == 1,
              "ONLY the active document's edit belongs in this commit - an entity from "
              "an additively streamed scene is another file's content, and shipping it "
              "here would apply that file's edits to this one");
        if (cs.History().Commits().size() == 1) {
            const collab::Commit& c = cs.History().Commits()[0];
            bool sawStreamed = false;
            for (const collab::Change& ch : c.changes)
                if (ch.guid == 2001) sawStreamed = true;
            Check(!sawStreamed, "the streamed entity leaked into this document's history");
            Check(c.parent.Valid() == false, "the first commit has no parent");
        }

        // The commit is durable, and reopening picks it up.
        CollabSession again;
        Check(again.EnsureIdentity(dir / "a.hbkey"), "reload the identity");
        again.NoteSceneOpened(s, scenePath, &ActiveOnly);
        Check(again.History().Commits().size() == 1,
              "history must survive being reopened - it is a file, not a session");

        // A second change set names the first as its parent.
        reg.get<Name>(also).value = "Second";
        cs.SealCommit(s, "again", &why);
        Check(cs.History().Commits().size() == 2, "a second change set should be recorded");
        if (cs.History().Commits().size() == 2)
            Check(cs.History().Commits()[1].parent == cs.History().Commits()[0].id,
                  "the second change set must name the first as its parent");
    }

    // ================================================================
    // PART 2 - REVIEW. A conflict is held until a person answers it.
    // ================================================================
    {
        Scene s;
        entt::registry& reg = s.Registry();
        const entt::entity e = s.CreateEntity("Thing");
        reg.emplace_or_replace<Guid>(e, Guid{7});
        s.Environment().docId = 42;
        s.Environment().guidEpoch = 99;

        CollabSession cs;
        cs.EnsureIdentity(dir / "b.hbkey");
        cs.NoteSceneOpened(s, dir / "R.hbscene", &ActiveOnly);
        reg.get<Name>(e).value = "Mine";
        std::string why;
        cs.SealCommit(s, "mine", &why);

        // Their history: the same entity, a different value, from the same base.
        collab::Journal theirs;
        collab::Commit t;
        t.id = collab::CommitId{999, 0};
        t.doc = 42;
        t.guidEpoch = 99;
        t.author = "ben";
        t.changes = {{7, "name", collab::ChangeOp::Set, "\"Thing\"", "\"Theirs\""}};
        theirs.Add(t);

        Check(cs.BeginReview(theirs, s), "a review should start");
        Check(cs.HasReview(), "there should be a review pending");
        Check(cs.ReviewPlan().verdict == collab::MergeVerdict::NeedsReview,
              "the same entity edited twice needs a person");
        Check(!cs.AllDecided(), "a fresh review starts undecided");
        Check(cs.ApplyReview(s, &why) == 0 && !why.empty(),
              "an undecided review must apply NOTHING and say why");
        Check(reg.get<Name>(e).value == "Mine", "an undecided review changed the scene");

        cs.Decisions()[0] = scene::Resolution::TakeTheirs;
        Check(cs.AllDecided(), "the review is answered now");
        Check(cs.ApplyReview(s, &why) == 1, "a decided review should apply");
        Check(reg.get<Name>(e).value == "Theirs", "take-theirs did not land");
        Check(!cs.HasReview(), "an applied review should be cleared");
    }

    // ================================================================
    // PART 3 - THE INVITE FLOW, over a real peer-to-peer link.
    // No ICE servers: host candidates connect two peers on one machine,
    // so this needs no internet.
    // ================================================================
    {
        const fs::path hostProj = dir / "hostproj";
        fs::create_directories(hostProj, ec);
        collab::IceConfig localOnly;

        CollabSession host, guest;
        Check(host.EnsureIdentity(dir / "host.hbkey"), "host identity");
        Check(guest.EnsureIdentity(dir / "guest.hbkey"), "guest identity");
        host.SetIceConfig(localOnly);
        guest.SetIceConfig(localOnly);
        host.SetProjectRoot(hostProj);
        Check(host.People().empty(), "a new project lets nobody in");

        const auto pump = [&]() {
            host.Tick(0);
            guest.Tick(0);
        };

        Check(host.StartHosting(), "hosting should start");
        Check(host.CreateInvitation(), "an invitation should start");
        Check(WaitUntil([&] { return !host.Invitation().empty(); }, 10000, pump),
              "the invitation never became ready");

        // The guest sees WHO it claims to be from before anything is trusted.
        Check(guest.PrepareJoin(host.Invitation()), "the guest should accept the text");
        Check(guest.Role() == SessionRole::Confirming,
              "the guest must stop and ask before connecting");
        Check(guest.PendingHostFingerprint() == host.MyFingerprint(),
              "the invitation must advertise the host's real fingerprint");
        Check(guest.ConfirmJoin(), "the guest should connect once confirmed");
        Check(WaitUntil([&] { return !guest.Reply().empty(); }, 10000, pump),
              "the reply never became ready");
        Check(host.AcceptReply(guest.Reply()), "the host should accept the reply");

        // NOT ON THE LIST. The guest's crypto is fine; it is simply not invited.
        WaitUntil([&] { return !host.Knocks().empty(); }, 15000, pump);
        Check(!guest.Live(), "an UNINVITED guest must not get in");
        Check(host.PeerCount() == 0, "an uninvited guest must not become a session");
        Check(host.Knocks().size() == 1,
              "the host must be SHOWN who tried - without it there is no way to invite "
              "anyone and the feature looks broken");
        if (!host.Knocks().empty()) {
            Check(host.Knocks()[0].key == guest.Me().Public(),
                  "the knock must carry the key the guest actually proved");
            Check(host.Knocks()[0].fingerprint == guest.MyFingerprint(),
                  "the fingerprint shown must be the guest's real one");

            // The host lets them in. This is the whole invite flow.
            Check(host.AllowPerson(host.Knocks()[0].key, "ana"), "adding a person");
        }
        Check(host.People().size() == 1, "the person should be on the list");
        Check(host.Knocks().empty(), "an admitted person is no longer knocking");
        Check(fs::exists(AllowlistFile(hostProj)),
              "the list must be written IMMEDIATELY - a crash must not forget a "
              "colleague who could join yesterday");

        // It survives a reload, which is what makes it worth writing.
        CollabSession reopened;
        reopened.SetProjectRoot(hostProj);
        Check(reopened.People().size() == 1, "the list must survive reopening the project");

        // And now they can actually connect.
        host.Leave();
        guest.Leave();
        CollabSession host2, guest2;
        host2.EnsureIdentity(dir / "host.hbkey");
        guest2.EnsureIdentity(dir / "guest.hbkey");
        host2.SetIceConfig(localOnly);
        guest2.SetIceConfig(localOnly);
        host2.SetProjectRoot(hostProj);
        const auto pump2 = [&]() {
            host2.Tick(0);
            guest2.Tick(0);
        };
        Check(host2.StartHosting() && host2.CreateInvitation(), "second session starts");
        Check(WaitUntil([&] { return !host2.Invitation().empty(); }, 10000, pump2),
              "second invitation never became ready");
        Check(guest2.PrepareJoin(host2.Invitation()) && guest2.ConfirmJoin(), "guest joins");
        Check(WaitUntil([&] { return !guest2.Reply().empty(); }, 10000, pump2),
              "second reply never became ready");
        Check(host2.AcceptReply(guest2.Reply()), "host accepts");
        Check(WaitUntil([&] { return guest2.Live(); }, 20000, pump2),
              "an INVITED guest must connect");
        if (!guest2.Live()) std::printf("  guest: %s\n", guest2.Status().c_str());
        Check(host2.PeerCount() == 1, "the host should see the guest");

        // ============================================================
        // FROM NOTHING. The guest has an EMPTY FOLDER and ends up with the
        // whole project, over the same authenticated link.
        // ============================================================
        {
            WriteTestFile(hostProj / "Game.hbproj", "{\"name\":\"Game\"}");
            WriteTestFile(hostProj / "Assets" / "Scenes" / "Level.hbscene", "scene bytes here");
            WriteTestFile(hostProj / "Assets" / "empty.txt", "");
            // Two things that must NOT travel: build output, and the host's own roster.
            WriteTestFile(hostProj / "Build" / "Game.exe", "build output");
            // A file big enough to cross several chunks, so the chunking is exercised
            // rather than assumed.
            std::string big;
            big.reserve(collab::kFileChunkBytes * 2 + 1234);
            for (usize i = 0; i < collab::kFileChunkBytes * 2 + 1234; ++i)
                big.push_back(static_cast<char>('a' + (i % 26)));
            WriteTestFile(hostProj / "Assets" / "big.bin", big);

            Check(host2.ShareProject(), "the host should be able to share its project");
            Check(host2.SharedFileCount() >= 4, "the manifest should list the project files");

            const fs::path landing = dir / "fresh";
            Check(guest2.StartDownload(landing), "the guest should start a download");
            const bool finished = WaitUntil(
                [&] {
                    return guest2.Download() == CollabSession::DownloadPhase::Done ||
                           guest2.Download() == CollabSession::DownloadPhase::Failed;
                },
                40000, pump2);
            Check(finished, "the download never finished");
            if (guest2.Download() == CollabSession::DownloadPhase::Failed)
                std::printf("  download failed: %s\n", guest2.DownloadError().c_str());
            Check(guest2.Download() == CollabSession::DownloadPhase::Done,
                  "THE PROJECT DID NOT ARRIVE - a peer cannot start from nothing");

            Check(fs::exists(landing / "Game.hbproj"), "the project file should have landed");
            Check(fs::exists(landing / "Assets" / "Scenes" / "Level.hbscene"),
                  "the scene should have landed");
            Check(fs::exists(landing / "Assets" / "empty.txt"),
                  "a ZERO-BYTE file must land too - it never produces a data chunk");
            {
                std::ifstream in(landing / "Assets" / "big.bin", std::ios::binary);
                const std::string got((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
                Check(got == big,
                      "a multi-chunk file arrived CORRUPTED - the chunk offsets or the "
                      "final marker are wrong");
            }
            Check(!fs::exists(landing / "Build"),
                  "build output must not have travelled");
            Check(!fs::exists(landing / "collab_authorized.txt"),
                  "the host's own access list must NEVER travel - it would hand over the "
                  "roster and let a guest's copy overwrite it later");
            Check(!fs::exists(dir / "fresh.incoming"),
                  "the staging folder should be gone once the transfer committed");

            // ASKING AGAIN when nothing changed must copy nothing.
            const fs::path landing2 = dir / "fresh2";
            fs::create_directories(landing2, ec);
            for (const char* f : {"Game.hbproj"})
                fs::copy_file(hostProj / f, landing2 / f, fs::copy_options::overwrite_existing, ec);
            Check(guest2.StartDownload(landing2), "a second download should start");
            WaitUntil(
                [&] {
                    return guest2.Download() == CollabSession::DownloadPhase::Done ||
                           guest2.Download() == CollabSession::DownloadPhase::Failed;
                },
                40000, pump2);
            Check(guest2.Download() == CollabSession::DownloadPhase::Done,
                  "the second download should succeed");
            Check(guest2.DownloadFilesTotal() < host2.SharedFileCount(),
                  "a file we ALREADY have must not be re-sent - the manifest is "
                  "content-addressed precisely so a resume is cheap");
        }

        // ============================================================
        // REAL TIME. The same object on both machines; one person moves
        // it and renames it, and it moves on the other screen without
        // anybody saving anything.
        // ============================================================
        Scene hs, gs;
        constexpr u64 kGuid = 909090;
        constexpr u64 kDoc = 0x5150, kEpoch = 0x7777;
        entt::entity he = entt::null, ge = entt::null;
        for (auto* pair : {&hs, &gs}) {
            Scene& sc = *pair;
            sc.Environment().docId = kDoc;
            sc.Environment().guidEpoch = kEpoch;
            const entt::entity e = sc.CreateEntity("Crate");
            sc.Registry().emplace_or_replace<Guid>(e, Guid{kGuid});
            Transform t;
            t.position = {0.0f, 0.0f, 0.0f};
            sc.Registry().emplace_or_replace<Transform>(e, t);
            (&sc == &hs ? he : ge) = e;
        }
        host2.NoteSceneOpened(hs, dir / "live.hbscene", &ActiveOnly);
        guest2.NoteSceneOpened(gs, dir / "live.hbscene", &ActiveOnly);

        u64 clock = 100000;
        const auto liveTick = [&]() {
            clock += 16;
            host2.LiveSync(hs, he, clock);
            guest2.LiveSync(gs, ge, clock);
            host2.Tick(clock);
            guest2.Tick(clock);
        };

        // The host selects the crate; the guest selects nothing, so only one side
        // asks for the lock.
        for (int i = 0; i < 10; ++i) {
            clock += 16;
            host2.LiveSync(hs, he, clock);
            guest2.LiveSync(gs, entt::null, clock);
            host2.Tick(clock);
            guest2.Tick(clock);
        }
        Check(host2.CanEdit(kGuid), "the host should hold the object it selected");

        // Move it and rename it - TWO components in the same frame, the case that
        // used to lose the second one.
        hs.Registry().get<Transform>(he).position = {5.0f, 1.0f, -2.0f};
        hs.Registry().get<Name>(he).value = "MovedCrate";
        for (int i = 0; i < 12; ++i) {
            clock += 16;
            host2.LiveSync(hs, he, clock);
            guest2.LiveSync(gs, entt::null, clock);
            host2.Tick(clock);
            guest2.Tick(clock);
        }

        const glm::vec3 gp = gs.Registry().get<Transform>(ge).position;
        Check(std::abs(gp.x - 5.0f) < 1e-3f && std::abs(gp.y - 1.0f) < 1e-3f &&
                  std::abs(gp.z + 2.0f) < 1e-3f,
              "THE MOVE DID NOT REACH THE OTHER MACHINE - real-time editing is not "
              "actually live");
        Check(gs.Registry().get<Name>(ge).value == "MovedCrate",
              "the rename did not reach the other machine - the second component of a "
              "two-component change was lost");

        // NO ECHO. Now that the guest has applied the host's change, neither side may
        // send it back: a change that bounces is an infinite loop that looks like lag.
        const collab::Seq before = 0;
        (void)before;
        const usize hostAppliedBefore = host2.AppliedThisFrame();
        (void)hostAppliedBefore;
        for (int i = 0; i < 6; ++i) liveTick();
        Check(host2.AppliedThisFrame() == 0 && guest2.AppliedThisFrame() == 0,
              "a change is still bouncing between the two peers - the applied value was "
              "not folded into the sender's baseline");

        // THE OTHER DIRECTION, and the lock moving with the selection. The host
        // deselects; the guest selects and edits.
        // 100 ms a tick: the lock retry is throttled to twice a second on purpose, so a
        // handover takes wall time rather than frames.
        for (int i = 0; i < 60; ++i) {
            clock += 100;
            host2.LiveSync(hs, entt::null, clock);
            guest2.LiveSync(gs, ge, clock);
            host2.Tick(clock);
            guest2.Tick(clock);
        }
        Check(guest2.CanEdit(kGuid),
              "the lock did not follow the selection - the guest could not take an "
              "object the host stopped holding");
        gs.Registry().get<Transform>(ge).position = {-3.0f, 0.0f, 0.0f};
        for (int i = 0; i < 40; ++i) {
            clock += 100;
            host2.LiveSync(hs, entt::null, clock);
            guest2.LiveSync(gs, ge, clock);
            host2.Tick(clock);
            guest2.Tick(clock);
        }
        Check(std::abs(hs.Registry().get<Transform>(he).position.x + 3.0f) < 1e-3f,
              "the GUEST's edit did not reach the host - the authority is the one peer "
              "nothing broadcasts back to, and it needs its own hook");

        // Bad text is named, not silently ignored.
        CollabSession g3;
        g3.EnsureIdentity(dir / "g3.hbkey");
        Check(!g3.PrepareJoin("nonsense"), "arbitrary text must be refused");
        Check(!g3.PrepareJoin(guest2.Reply()),
              "a REPLY pasted where an invitation belongs must be refused");
        host2.Leave();
        guest2.Leave();
    }

    fs::remove_all(dir, ec);
    if (g_csFails == 0) {
        std::printf("collabsession: a peer with an EMPTY FOLDER receives the whole project "
                    "over the link - multi-chunk files byte-exact, zero-byte files "
                    "intact, build output and the host's roster left behind, and a second "
                    "run re-sending only what differs; a move and a rename made on one "
                    "other with nobody saving, the lock follows the selection so the "
                    "edit can go back the other way, and nothing bounces; a save records "
                    "only the ACTIVE document's changes "
                    "(a streamed entity stays out), an unchanged save records nothing, "
                    "history survives reopening and chains its parents; a conflict "
                    "applies nothing until answered; and over a real peer-to-peer link "
                    "an uninvited guest is refused but SHOWN to the host, who admits "
                    "them and then they connect\n");
    }
    return g_csFails == 0;
}

void CollabSession::DismissReview() {
    hasReview_ = false;
    review_ = collab::MergePlan();
    decisions_.clear();
}

} // namespace hbe::editor
