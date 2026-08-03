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
    server_->onLived = [this](const collab::MsgEntityLived& l) { QueueLife(l); };
    server_->onPainted = [this](const collab::MsgPaintCommitted& p) { QueuePaint(p); };
    role_ = SessionRole::Hosting;
    // HOSTING IS SHARING. Until now a SECOND, separate click was needed before a joiner
    // could receive a single byte, and nothing on either screen named it. A host that has
    // not shared answers every SyncRequest with an EMPTY manifest, which the far end
    // reports as a COMPLETED COPY OF NOTHING - so both people see success and no files.
    // Worse, the Hub asks automatically the moment the link opens, so the host had no
    // window in which to click in time: across two machines the invitation is ferried by
    // chat over minutes, and "People connected: 1" is the cue that finally prompts the
    // click - by which point the empty answer has already been sent, and nothing ever
    // asks again. ShareProject() is a no-op without a project root; the button and
    // "Rescan" remain for files added later.
    ShareProject(); // writes status_ itself, so it MUST run before the line below
    status_ = shared_.files.empty()
                  ? std::string("Hosting. Create an invitation to add someone.")
                  : "Hosting " + std::to_string(shared_.files.size()) +
                        " file(s). Create an invitation to add someone.";
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

collab::LinkState CollabSession::GuestLinkState() const {
    return guest_ ? guest_->State() : collab::LinkState::Closed;
}

const char* CollabSession::GuestLinkError() const {
    return guest_ ? guest_->Error() : "";
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
    server_.reset(); // shareRoot_/shareManifest_ die with it...
    host_.reset();
    // ...so this must not keep claiming otherwise. SharedFileCount() is what hides the
    // Share button, so a stale value showed a green "Sharing N file(s)" over a brand-new
    // server that was sharing nothing - the "worked once, never again" shape.
    shared_.files.clear();
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
    // A stalled download has to fail on its own; nothing upstream reports a refusal.
    fetch_.Tick(nowMs);
    if (host_) {
        host_->Poll();
        if (server_) server_->Tick(nowMs);
        host_->Poll();
        if (invite_ != 0 && invitation_.empty() &&
            host_->StateOf(invite_) == collab::LinkState::WaitingForPeer) {
            invitation_ = host_->Invitation(invite_);
            status_ = "Invitation ready - send it to them.";
        }
        // REPORT EVERY STATE, not just success.
        //
        // This used to react only to Open, so ANY other outcome left the host reading
        // "Connecting..." forever: a link that fails ICE, or one that is dropped because
        // the guest was refused, is REMOVED from the transport - StateOf then answers
        // Closed and nothing was looking. The person is left staring at a word that is no
        // longer true, on a screen with nothing else to go on.
        if (invite_ != 0) {
            const collab::LinkState st = host_->StateOf(invite_);
            if (st != lastInviteState_) {
                lastInviteState_ = st;
                switch (st) {
                case collab::LinkState::Open:
                    status_ = "Connected. " + std::to_string(PeerCount()) + " here.";
                    break;
                case collab::LinkState::Connecting:
                    status_ = "Connecting to them...";
                    break;
                case collab::LinkState::Failed:
                case collab::LinkState::Closed:
                    // Do NOT clobber a knock. "Someone tried to join <fingerprint>" is
                    // the single most useful thing the host can be told, and it is set by
                    // the policy callback a moment before the link is dropped.
                    if (knocks_.empty()) {
                        status_ = "That connection did not come up. Create a new "
                                  "invitation and try again - an invitation goes stale "
                                  "once the addresses in it change.";
                    }
                    // Let them start over; the old invitation is dead either way.
                    invite_ = 0;
                    invitation_.clear();
                    break;
                default:
                    break;
                }
            }
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
        } else if (guest_->State() == collab::LinkState::Failed ||
                   guest_->State() == collab::LinkState::Closed) {
            // Closed covers a link that dies after opening; Failed only ever covers one
            // that never opened. Testing Failed alone left a dropped session reading
            // "Connected." forever.
            const char* why = guest_->Error();
            status_ = (why && *why)
                          ? std::string("Could not connect: ") + why
                          : std::string("The connection dropped. Ask them for a fresh "
                                        "invitation and try again.");
            role_ = SessionRole::Offline;
            client_.reset();
            guest_.reset();
        }
    }
}

// --- getting the whole project --------------------------------------------------


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
    // DELEGATED, not reimplemented. The Hub has to run exactly this without an engine
    // attached, so the state machine lives in Collab/ProjectFetch and both drive the same
    // one - two copies would agree today and drift quietly afterwards.
    fetch_.Attach(client_.get());
    downloadRoot_ = into;
    if (!fetch_.Start(into)) return false;
    status_ = "Asking what they have...";
    return true;
}

void CollabSession::OnManifest(const collab::MsgSyncManifest& m) {
    fetch_.OnManifest(m);
    // Prefer the REASON over the phase name. "Project: Done" is what an empty manifest
    // used to print - technically true and completely useless.
    status_ = !fetch_.Error().empty()
                  ? fetch_.Error()
                  : std::string("Project: ") + collab::ProjectFetch::PhaseName(fetch_.State());
}

void CollabSession::OnFileChunk(const collab::MsgFileChunk& c) {
    fetch_.OnChunk(c);
    if (fetch_.State() == collab::ProjectFetch::Phase::Downloading)
        status_ = "Copying... " + std::to_string(fetch_.FilesDone()) + "/" +
                  std::to_string(fetch_.FilesTotal());
    else if (fetch_.State() == collab::ProjectFetch::Phase::Done)
        status_ = "Copied " + std::to_string(fetch_.FilesTotal()) + " file(s).";
}

// --- real time ------------------------------------------------------------------

void CollabSession::QueueRemote(const collab::MsgDeltaApplied& d) {
    // Queued, never applied here. These callbacks fire from inside CollabClient::Pump /
    // CollabServer::Tick, and touching the registry there would mutate the scene from
    // the middle of a network drain - the single-threaded ordering the whole layer
    // depends on. LiveSync drains this at ONE defined point in the frame.
    remote_.push_back(d);
}

void CollabSession::QueueLife(const collab::MsgEntityLived& l) { remoteLife_.push_back(l); }

void CollabSession::QueuePaint(const collab::MsgPaintCommitted& p) { remotePaint_.push_back(p); }

void CollabSession::SendStroke(const std::string& source, const paint::Stroke& s) {
    if (!Live()) return;
    const u64 canvas = paint::CanvasIdOf(source);
    // A canvas that has never been saved has no identity anyone else can resolve, so
    // there is nothing to send it TO. Silently dropping would be worse than not trying:
    // the artist would see their strokes never arrive with no reason given.
    if (canvas == 0) {
        status_ = "Save this canvas once before your strokes can be shared.";
        return;
    }
    const std::vector<u8> blob = paint::EncodeStroke(s);
    if (blob.empty()) return;
    if (server_) {
        // The host is the authority and does not go through the wire. Commit it to the
        // paint history and broadcast, exactly as OnPaintOp would for a guest.
        server_->LocalPaint(canvas, s.layerId, blob);
    } else if (client_) {
        client_->SendPaintOp(canvas, s.layerId, blob);
    }
}

void CollabSession::WireCallbacks() {
    callbacks_.onLived = [this](const collab::MsgEntityLived& l) { QueueLife(l); };
    callbacks_.onPaint = [this](const collab::MsgPaintCommitted& p) { QueuePaint(p); };
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

    // --- 1. entities that appeared or disappeared elsewhere ------------------
    //
    // BEFORE the component deltas, and that ordering is load-bearing. A newly created
    // object's creation and its components are sent in the SAME tick, so applying deltas
    // first means every one of them names a guid this scene does not have yet - they are
    // skipped, and the far side is left holding a nameless empty object that looks like a
    // corrupt scene.
    for (const collab::MsgEntityLived& l : remoteLife_) {
        if (l.key.doc != docId_) continue;
        entt::entity found = entt::null;
        for (const entt::entity e : reg.view<Guid>())
            if (reg.get<Guid>(e).value == l.key.guid) { found = e; break; }
        if (l.destroy) {
            if (found != entt::null) {
                // ONE entity, matching what the message means. Children are not implied:
                // hierarchy rides on `parent`, which is a file row index and cannot cross
                // machines, so a recursive delete here would guess - and guess wrong.
                // The editor deletes children explicitly, and each arrives as its own
                // message.
                reg.destroy(found);
                ++appliedThisFrame_;
            }
            knownGuids_.erase(l.key.guid);
            if (lockedGuid_ == l.key.guid) {
                // The thing we were holding is gone. Forget it, or the next diff would
                // resurrect it component by component.
                lockedGuid_ = 0;
                liveBaseline_.clear();
            }
        } else if (found == entt::null) {
            const entt::entity e = s.CreateEntity(l.name.empty() ? "Object" : l.name);
            // The guid is the identity, so it must be THEIRS, not a fresh one - the two
            // copies would otherwise never refer to the same object again.
            reg.emplace_or_replace<Guid>(e, Guid{l.key.guid});
            knownGuids_[l.key.guid] = 1;
            ++appliedThisFrame_;
        }
    }
    remoteLife_.clear();


    // --- 1b. strokes other artists painted -----------------------------------
    //
    // DELIBERATELY NOT THROUGH Editor::CommitStroke. That pushes onto the local paint
    // undo stack and clears the redo stack, so a colleague's stroke would land in YOUR
    // Ctrl+Z history - undoing your own work would silently undo theirs instead, and
    // their stroke would come off a canvas they are still painting on.
    strokesApplied_ = 0;
    for (const collab::MsgPaintCommitted& msg : remotePaint_) {
        entt::entity target = entt::null;
        PaintComponent* pc = nullptr;
        for (const entt::entity e : reg.view<PaintComponent>()) {
            PaintComponent& c = reg.get<PaintComponent>(e);
            if (paint::CanvasIdOf(c.source) == msg.canvas) {
                target = e;
                pc = &c;
                break;
            }
        }
        if (!pc) continue; // a canvas this copy does not have open
        // A pre-v3 canvas was loaded as baked pixels with NO stroke history, and
        // BakeFromStrokes refuses to run on one. Appending here would put the stroke in
        // the list and produce no paint at all - the artist would see nothing happen and
        // have no idea why.
        if (!pc->strokesComplete) {
            status_ = "A collaborator painted on a canvas that has no editable history "
                      "here, so it could not be applied. Re-save it to upgrade it.";
            continue;
        }
        paint::Stroke s;
        if (!paint::DecodeStroke(msg.strokeBlob.data(), msg.strokeBlob.size(), s)) continue;
        pc->strokes.push_back(std::move(s));
        // Pure CPU. The GPU upload is paint::Sync, which the editor already runs from its
        // own draw path - doing it here would need a Renderer this layer does not have.
        paint::BakeFromStrokes(*pc);
        pc->dirty = true;
        (void)target;
        ++strokesApplied_;
    }
    remotePaint_.clear();

    // --- 2. apply what other people changed ----------------------------------
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

    // --- 1c. entities that appeared or disappeared HERE ----------------------
    {
        std::unordered_map<u64, char> now;
        for (const entt::entity e : reg.view<Guid>()) {
            if (include_ && !include_(reg, e)) continue; // another document's entity
            const u64 g = reg.get<Guid>(e).value;
            if (g != 0) now[g] = 1;
        }
        if (!guidsSeeded_) {
            // First frame of a session: everything is "already there", not "just
            // created". Without this, joining would broadcast the entire scene as new.
            knownGuids_ = now;
            guidsSeeded_ = true;
        } else {
            for (const auto& [g, _] : now) {
                if (knownGuids_.count(g)) continue;
                collab::EntityKey k;
                k.doc = docId_;
                k.guid = g;
                entt::entity e = entt::null;
                for (const entt::entity c : reg.view<Guid>())
                    if (reg.get<Guid>(c).value == g) { e = c; break; }
                const Name* nm = (e != entt::null) ? reg.try_get<Name>(e) : nullptr;
                const std::string label = nm ? nm->value : std::string();
                if (server_) server_->LocalLife(k, false, label);
                else if (client_) client_->SendLife(k, false, label);
                // A new object arrives with no components on the far side, so push its
                // whole state once. It is one entity, and waiting for the author to
                // select it would leave a nameless empty object on their screen.
                if (e != entt::null) {
                    if (server_) server_->LocalLock(k, nowMs);
                    for (const std::string& key : scene::DeltaComponentKeys()) {
                        std::string j;
                        if (!scene::ComponentToJson(s, e, key, j)) continue;
                        if (server_) server_->LocalDelta(k, key, j);
                        else client_->SendDelta(k, key, j);
                    }
                    if (server_) server_->LocalUnlock(k);
                }
            }
            for (const auto& [g, _] : knownGuids_) {
                if (now.count(g)) continue;
                collab::EntityKey k;
                k.doc = docId_;
                k.guid = g;
                // Deleting needs the lock. Take it first: the author deleted it locally
                // already, and without the lock the far side would keep the object.
                if (server_) {
                    server_->LocalLock(k, nowMs);
                    server_->LocalLife(k, true, std::string());
                    server_->LocalUnlock(k);
                } else if (client_) {
                    client_->RequestLock(k);
                    client_->SendLife(k, true, std::string());
                }
            }
            knownGuids_ = std::move(now);
        }
    }

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
    // A different scene: everything in it is 'already there', not newly created.
    guidsSeeded_ = false;
    knownGuids_.clear();
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
    // PART 1b - THE STROKE CODEC. One field order for the file and the
    // wire; a stroke that survives the trip byte-for-byte is the whole
    // precondition for two artists seeing the same painting.
    // ================================================================
    {
        paint::Stroke s;
        s.type = paint::StrokeType::Path;
        s.layer = 2;
        s.layerId = 77;
        s.projection = 1;
        s.brush.name = "Round soft";
        s.brush.hardness = 0.33f;
        s.brush.customSize = 2;
        s.brush.customAlpha = {1, 2, 3, 4};
        s.color = {0.25f, 0.5f, 0.75f, 1.0f};
        s.metallic = 0.1f;
        s.roughness = 0.2f;
        s.erase = true;
        s.paintMaterial = false;
        for (int i = 0; i < 4; ++i) {
            paint::StrokePoint pt;
            pt.uv = {0.1f * i, 0.2f * i};
            pt.radius = 0.05f + 0.01f * i;
            pt.pressure = 0.5f;
            pt.localPos = {1.0f * i, 2.0f, 3.0f};
            pt.localNormal = {0.0f, 1.0f, 0.0f};
            pt.localRadius = 0.4f;
            s.path.push_back(pt);
        }
        const std::vector<u8> blob = paint::EncodeStroke(s);
        Check(!blob.empty(), "a stroke should encode");
        paint::Stroke back;
        Check(paint::DecodeStroke(blob.data(), blob.size(), back),
              "a stroke should decode");
        Check(back.layerId == s.layerId,
              "the STABLE layer id must survive - an index cannot, and a stroke that "
              "lands on the wrong layer is worse than one that does not land");
        Check(back.path.size() == s.path.size(), "the path length must survive");
        Check(back.brush.name == s.brush.name && back.brush.customAlpha == s.brush.customAlpha,
              "the brush, including a custom tip, must survive or the stroke paints "
              "different marks on the other machine");
        Check(back.erase == s.erase && back.paintMaterial == s.paintMaterial,
              "the stroke's flags must survive");
        Check(!back.path.empty() &&
                  std::abs(back.path[3].radius - s.path[3].radius) < 1e-6f &&
                  std::abs(back.path[3].localPos.x - s.path[3].localPos.x) < 1e-6f,
              "every point field must survive");
        // Junk and truncation are refused, never half-read.
        paint::Stroke junk;
        Check(!paint::DecodeStroke(blob.data(), 3, junk),
              "a truncated stroke must be refused");
        std::vector<u8> wrongVersion = blob;
        wrongVersion[0] = 0xEE;
        Check(!paint::DecodeStroke(wrongVersion.data(), wrongVersion.size(), junk),
              "a stroke from an unknown build must be REFUSED - BinaryReader treats "
              "trailing bytes as success, so a half-understood stroke would paint "
              "plausible-looking wrong marks");

        // The canvas id: derived from the path, stable, and case/slash insensitive.
        Check(paint::CanvasIdOf("Art/Wall.hbpaint") == paint::CanvasIdOf("art\\Wall.hbpaint"),
              "the same canvas reached by two spellings must be ONE canvas - Windows "
              "paths make both routine");
        Check(paint::CanvasIdOf("Art/Wall.hbpaint") != paint::CanvasIdOf("Art/Floor.hbpaint"),
              "two canvases must not collide");
        Check(paint::CanvasIdOf("") == 0,
              "an unsaved canvas has no identity anyone else could resolve");
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

        // ============================================================
        // ADDING AND REMOVING OBJECTS. No component delta can say this:
        // a delta mutates a component of an entity the far side must
        // already have, and empty json removes a COMPONENT, not the
        // object. Without it, adding or deleting simply did not travel
        // and the two scenes silently diverged until someone saved.
        // ============================================================
        constexpr u64 kNewGuid = 555001;
        {
            const entt::entity fresh = hs.CreateEntity("SpawnedByHost");
            hs.Registry().emplace_or_replace<Guid>(fresh, Guid{kNewGuid});
            Transform t;
            t.position = {7.0f, 8.0f, 9.0f};
            hs.Registry().emplace_or_replace<Transform>(fresh, t);

            entt::entity landed = entt::null;
            WaitUntil(
                [&] {
                    for (const entt::entity e : gs.Registry().view<Guid>())
                        if (gs.Registry().get<Guid>(e).value == kNewGuid) {
                            landed = e;
                            return true;
                        }
                    return false;
                },
                20000,
                [&] {
                    clock += 100;
                    host2.LiveSync(hs, fresh, clock);
                    guest2.LiveSync(gs, entt::null, clock);
                    host2.Tick(clock);
                    guest2.Tick(clock);
                });
            Check(landed != entt::null,
                  "A NEW OBJECT DID NOT APPEAR on the other machine");
            if (landed != entt::null) {
                Check(gs.Registry().get<Name>(landed).value == "SpawnedByHost",
                      "the new object arrived without its name");
                // Its components must come with it - an empty nameless object would be
                // worse than nothing, because it looks like a bug in the scene.
                const bool posed = WaitUntil(
                    [&] {
                        const Transform* gt = gs.Registry().try_get<Transform>(landed);
                        return gt && std::abs(gt->position.x - 7.0f) < 1e-3f;
                    },
                    20000,
                    [&] {
                        clock += 100;
                        host2.LiveSync(hs, fresh, clock);
                        guest2.LiveSync(gs, entt::null, clock);
                        host2.Tick(clock);
                        guest2.Tick(clock);
                    });
                Check(posed,
                      "a newly created object arrived with no components - the far side "
                      "gets an empty placeholder that looks like a corrupt scene");
            }

            // ...and DELETING it removes it there too.
            hs.Registry().destroy(fresh);
            const bool gone = WaitUntil(
                [&] {
                    for (const entt::entity e : gs.Registry().view<Guid>())
                        if (gs.Registry().get<Guid>(e).value == kNewGuid) return false;
                    return true;
                },
                20000,
                [&] {
                    clock += 100;
                    host2.LiveSync(hs, entt::null, clock);
                    guest2.LiveSync(gs, entt::null, clock);
                    host2.Tick(clock);
                    guest2.Tick(clock);
                });
            Check(gone, "A DELETED OBJECT SURVIVED on the other machine");

            // And it does not come back: a deletion that the sender then re-broadcasts
            // as a creation is an object that flickers forever.
            for (int i = 0; i < 10; ++i) {
                clock += 100;
                host2.LiveSync(hs, entt::null, clock);
                guest2.LiveSync(gs, entt::null, clock);
                host2.Tick(clock);
                guest2.Tick(clock);
            }
            bool reappeared = false;
            for (const entt::entity e : gs.Registry().view<Guid>())
                if (gs.Registry().get<Guid>(e).value == kNewGuid) reappeared = true;
            Check(!reappeared, "a deleted object came BACK - the deletion echoed");
        }

        // ============================================================
        // PAINTING TOGETHER. Two artists on one canvas is the workflow
        // the art build exists for, so paint is deliberately NOT
        // lock-gated - both sides' strokes must land, not race.
        // ============================================================
        {
            const std::string kCanvas = "Art/Wall.hbpaint";
            PaintComponent* hpc = nullptr;
            PaintComponent* gpc = nullptr;
            for (auto* pair : {&hs, &gs}) {
                Scene& sc = *pair;
                const entt::entity e = sc.CreateEntity("PaintedSurface");
                sc.Registry().emplace_or_replace<Guid>(e, Guid{424242});
                PaintComponent pc;
                pc.resolution = 32;
                pc.source = kCanvas;
                pc.strokesComplete = true;
                PaintLayer layer;
                layer.id = 1;
                layer.color.assign(32u * 32u * 4u, 0);
                layer.material.assign(32u * 32u * 4u, 0);
                pc.layers.push_back(std::move(layer));
                sc.Registry().emplace_or_replace<PaintComponent>(e, std::move(pc));
                (&sc == &hs ? hpc : gpc) = &sc.Registry().get<PaintComponent>(e);
            }
            Check(hpc && gpc, "both sides should have the canvas");

            paint::Stroke brush;
            brush.type = paint::StrokeType::Path;
            brush.layerId = 1;
            brush.color = {1.0f, 0.0f, 0.0f, 1.0f};
            paint::StrokePoint pt;
            pt.uv = {0.5f, 0.5f};
            pt.radius = 0.2f;
            pt.pressure = 1.0f;
            brush.path.push_back(pt);

            const usize guestBefore = gpc->strokes.size();
            hpc->strokes.push_back(brush);
            host2.SendStroke(kCanvas, hpc->strokes.back());

            const bool arrived = WaitUntil(
                [&] { return gpc->strokes.size() > guestBefore; }, 20000,
                [&] {
                    clock += 100;
                    host2.LiveSync(hs, entt::null, clock);
                    guest2.LiveSync(gs, entt::null, clock);
                    host2.Tick(clock);
                    guest2.Tick(clock);
                });
            Check(arrived, "A BRUSH STROKE DID NOT REACH THE OTHER ARTIST");
            if (arrived) {
                Check(gpc->strokes.back().layerId == 1,
                      "the stroke landed with the wrong layer id");
                Check(!gpc->strokes.back().path.empty() &&
                          std::abs(gpc->strokes.back().path[0].uv.x - 0.5f) < 1e-5f,
                      "the stroke arrived but its path is wrong");
            }
            // The host must not stamp its own stroke twice.
            const usize hostAfter = hpc->strokes.size();
            for (int i = 0; i < 6; ++i) {
                clock += 100;
                host2.LiveSync(hs, entt::null, clock);
                guest2.LiveSync(gs, entt::null, clock);
                host2.Tick(clock);
                guest2.Tick(clock);
            }
            Check(hpc->strokes.size() == hostAfter,
                  "the host applied its OWN stroke a second time - it is the authority "
                  "and already has it");

            // ...and the other direction, over the wire rather than the host shortcut.
            const usize hostBefore = hpc->strokes.size();
            paint::Stroke reply = brush;
            reply.color = {0.0f, 1.0f, 0.0f, 1.0f};
            gpc->strokes.push_back(reply);
            guest2.SendStroke(kCanvas, gpc->strokes.back());
            const bool back = WaitUntil(
                [&] { return hpc->strokes.size() > hostBefore; }, 20000,
                [&] {
                    clock += 100;
                    host2.LiveSync(hs, entt::null, clock);
                    guest2.LiveSync(gs, entt::null, clock);
                    host2.Tick(clock);
                    guest2.Tick(clock);
                });
            Check(back,
                  "a GUEST artist's stroke did not reach the host - paint has its own "
                  "channel and the host is the one peer nothing broadcasts back to");
        }

        // THE OTHER DIRECTION, which is a DIFFERENT code path: a guest's creation goes
        // over the wire to the server handler, not through the host's local shortcut.
        {
            constexpr u64 kGuestGuid = 555002;
            const entt::entity mk = gs.CreateEntity("SpawnedByGuest");
            gs.Registry().emplace_or_replace<Guid>(mk, Guid{kGuestGuid});
            const bool arrived = WaitUntil(
                [&] {
                    for (const entt::entity e : hs.Registry().view<Guid>())
                        if (hs.Registry().get<Guid>(e).value == kGuestGuid) return true;
                    return false;
                },
                20000,
                [&] {
                    clock += 100;
                    host2.LiveSync(hs, entt::null, clock);
                    guest2.LiveSync(gs, mk, clock);
                    host2.Tick(clock);
                    guest2.Tick(clock);
                });
            Check(arrived,
                  "a GUEST's new object did not reach the host - the wire path for "
                  "creation is separate from the host's own shortcut");
        }

        // Bad text is named, not silently ignored.
        CollabSession g3;
        g3.EnsureIdentity(dir / "g3.hbkey");
        Check(!g3.PrepareJoin("nonsense"), "arbitrary text must be refused");
        Check(!g3.PrepareJoin(guest2.Reply()),
              "a REPLY pasted where an invitation belongs must be refused");
        host2.Leave();
        guest2.Leave();
        // ============================================================
        // THE LOSING ORDER. Everything above shares BEFORE the guest asks,
        // which is the one ordering that always worked - so it could never
        // have caught the bug people actually hit. Across two machines the
        // invitation is ferried by chat over minutes and the Hub asks the
        // instant the link opens, so the ask ALWAYS arrives first. A host
        // that had not shared answered with an EMPTY manifest, which the far
        // end reported as a finished copy of nothing, and nothing ever asked
        // again. This runs LAST, and on its own sessions, because it has to
        // tear a host down to prove the second half.
        // ============================================================
        {
            CollabSession host3, guest3;
            host3.EnsureIdentity(dir / "host.hbkey");
            guest3.EnsureIdentity(dir / "guest.hbkey");
            host3.SetIceConfig(localOnly);
            guest3.SetIceConfig(localOnly);
            host3.SetProjectRoot(hostProj);
            const auto pump3 = [&]() {
                host3.Tick(0);
                guest3.Tick(0);
            };

            // HOSTING IS SHARING. There is no second click to forget.
            Check(host3.StartHosting(), "the third session should start hosting");
            Check(host3.SharedFileCount() > 0,
                  "HOSTING MUST IMPLY SHARING - a joiner cannot receive a single byte "
                  "until the host has shared, and nothing on either screen said so");

            Check(host3.CreateInvitation(), "third invitation");
            Check(WaitUntil([&] { return !host3.Invitation().empty(); }, 10000, pump3),
                  "third invitation never became ready");
            Check(guest3.PrepareJoin(host3.Invitation()) && guest3.ConfirmJoin(),
                  "guest joins the third session");
            Check(WaitUntil([&] { return !guest3.Reply().empty(); }, 10000, pump3),
                  "third reply never became ready");
            Check(host3.AcceptReply(guest3.Reply()), "host accepts the third reply");
            Check(WaitUntil([&] { return guest3.Live(); }, 20000, pump3),
                  "the third guest must connect");

            // The guest asks WITHOUT anybody ever pressing Share.
            const fs::path landing3 = dir / "unshared";
            Check(guest3.StartDownload(landing3), "the third download should start");
            Check(WaitUntil(
                      [&] {
                          return guest3.Download() == CollabSession::DownloadPhase::Done ||
                                 guest3.Download() == CollabSession::DownloadPhase::Failed;
                      },
                      40000, pump3),
                  "the third download never finished");
            if (guest3.Download() == CollabSession::DownloadPhase::Failed)
                std::printf("  unshared download failed: %s\n",
                            guest3.DownloadError().c_str());
            Check(fs::exists(landing3 / "Game.hbproj"),
                  "THE PROJECT MUST ARRIVE WITHOUT A SEPARATE SHARE CLICK - this is the "
                  "two-machine failure: connected, handshake clean, and no files");

            // And a re-host must not inherit the old share. SharedFileCount() is what
            // hides the Share button, so a stale value showed a green "Sharing N file(s)"
            // over a brand-new server that was serving nothing - "worked once, never
            // again". Re-hosting must also re-share by itself.
            host3.Leave();
            guest3.Leave();
            Check(host3.SharedFileCount() == 0,
                  "Leave() must clear the share, or a re-host silently serves nothing");
            Check(host3.StartHosting() && host3.SharedFileCount() > 0,
                  "a SECOND hosting session must share again on its own");
            host3.Leave();
        }
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
