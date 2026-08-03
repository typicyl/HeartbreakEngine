// Collab/CollabServer.cpp
#include "Collab/CollabServer.h"

#include <fstream>

namespace hbe::collab {

namespace {
const std::vector<MsgPaintCommitted> kNoHistory;
}

void CollabServer::Tick(u64 nowMs) {
    if (!transport_) return;
    lastTickMs_ = nowMs;

    std::vector<ConnId> fresh;
    transport_->PollNewConnections(fresh);
    for (const ConnId c : fresh) {
        Peer p;
        p.conn = c;
        peers_[c] = std::move(p);
    }

    // Drain and dispatch. The inbox PERSISTS across ticks because a transport may hand
    // over half a frame; SplitFrames consumes only whole ones and the remainder waits.
    std::vector<ConnId> doomed;
    for (auto& [conn, p] : peers_) {
        transport_->Receive(conn, p.inbox);
        if (p.inbox.empty()) continue;
        usize consumed = 0;
        bool fatal = false;
        const std::vector<Frame> frames =
            SplitFrames(p.inbox.data(), p.inbox.size(), consumed, fatal);
        for (const Frame& f : frames) HandleFrame(p, f, nowMs);
        p.inbox.erase(p.inbox.begin(), p.inbox.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (fatal) {
            // Unrecoverable framing. Dropping is the ONLY correct response: the stream
            // cannot resynchronise, so keeping the connection means spinning on the
            // same bytes forever while the buffer grows.
            doomed.push_back(conn);
        }
    }

    // Close anything whose stream went bad, THEN poll - so the transport reports the
    // drop and the normal disconnect path (peer-left, writer handoff) runs once.
    for (const ConnId c : doomed) transport_->Disconnect(c);

    std::vector<ConnId> gone;
    transport_->PollDisconnects(gone);
    for (const ConnId c : gone) {
        const auto it = peers_.find(c);
        if (it == peers_.end()) continue;
        const UserId leaving = it->second.user;
        // The lease is deliberately left running - a reconnect inside kLockTimeoutMs
        // reclaims it. See LockTable::SessionEnded.
        locks_.SessionEnded(it->second.session);
        peers_.erase(it);
        if (leaving != 0) {
            std::vector<u8> bytes;
            EncodePeerLeft(bytes, MsgPeerLeft{leaving});
            Broadcast(bytes);
            // Hand the writer role on, or the project becomes unsavable for everyone
            // the moment the writer's laptop closes.
            if (writer_ == leaving) {
                writer_ = 0;
                for (auto& [c2, p2] : peers_) {
                    if (p2.user == 0) continue;
                    writer_ = p2.user;
                    MsgWelcome w;
                    w.user = p2.user;
                    w.session = p2.session;
                    w.isWriter = true;
                    std::vector<u8> wb;
                    EncodeWelcome(wb, w);
                    SendTo(p2, wb);
                    break;
                }
            }
        }
    }

    // One chunk per peer per tick: paced, and interleaved with everything else rather
    // than blocking the session while a project transfers.
    PumpFileSends();

    // Leases last, so an expiry observed this tick is broadcast this tick.
    std::vector<LockView> expired;
    locks_.ExpireStale(nowMs, expired);
    for (const LockView& v : expired) {
        MsgLockGrant g;
        g.view = v;
        g.expired = true;
        std::vector<u8> bytes;
        EncodeLockGrant(bytes, g);
        Broadcast(bytes);
    }
}

void CollabServer::HandleFrame(Peer& p, const Frame& f, u64 nowMs) {
    // A KNOWN kind at an UNKNOWN version is more dangerous than an unknown kind: the
    // payload layout may have changed, and BinaryReader treats leftover bytes as
    // success, so a v2 frame reusing a v1 kind would decode into plausible garbage and
    // be acted on. Skip it - the frame is already consumed, so the stream stays in sync.
    if (f.version != kProtocolVersion) return;
    // An UNKNOWN message type is skipped, not fatal - that is what the length field in
    // the frame header buys. A newer client may send something this build predates.
    switch (f.type) {
        case MsgType::Hello:
            if (const auto m = DecodeHello(f.payload, f.size)) OnHello(p, *m, nowMs);
            break;
        case MsgType::JoinDoc:
            if (const auto m = DecodeJoinDoc(f.payload, f.size)) p.doc = m->doc;
            break;
        case MsgType::LockRequest:
            if (const auto m = DecodeLockRequest(f.payload, f.size)) OnLockRequest(p, *m, nowMs);
            break;
        case MsgType::LockRelease:
            if (const auto m = DecodeLockRelease(f.payload, f.size)) OnLockRelease(p, *m);
            break;
        case MsgType::Heartbeat:
            // The client's own clock hint is READ AND IGNORED for expiry on purpose:
            // a skewed or hostile client must not be able to extend its own lease.
            if (DecodeHeartbeat(f.payload, f.size)) locks_.Heartbeat(p.session, nowMs);
            break;
        case MsgType::EntityDelta:
            if (const auto m = DecodeEntityDelta(f.payload, f.size)) OnEntityDelta(p, *m);
            break;
        case MsgType::PaintOp:
            if (const auto m = DecodePaintOp(f.payload, f.size)) OnPaintOp(p, *m);
            break;
        case MsgType::PaintPreview:
            if (const auto m = DecodePaintPreview(f.payload, f.size)) OnPaintPreview(p, *m);
            break;
        case MsgType::EntityLife:
            if (const auto m = DecodeEntityLife(f.payload, f.size)) OnEntityLife(p, *m);
            break;
        case MsgType::SyncRequest:
            if (DecodeSyncRequest(f.payload, f.size)) OnSyncRequest(p);
            break;
        case MsgType::FileRequest:
            if (const auto m = DecodeFileRequest(f.payload, f.size)) OnFileRequest(p, *m);
            break;
        default:
            break;
    }
}

void CollabServer::OnHello(Peer& p, const MsgHello& m, u64 nowMs) {
    if (p.helloed) return; // a second Hello on one connection is not an identity change
    p.helloed = true;
    p.name = m.displayName;

    // Resume the same UserId for a returning name, so locks and history attribution
    // survive a reconnect. Identity is by NAME because nothing here authenticates -
    // which is precisely why this must not face an untrusted network until the Hub
    // owns real identity.
    // RESUME ONLY IF THAT IDENTITY IS NOT ALREADY ONLINE. Keying purely on the name
    // collapsed two SIMULTANEOUS connections into one UserId: both were told
    // isWriter=true, the second stole every lock the first held, and the first kept
    // reporting CanEdit()==true while its edits were refused. Two people who happen to
    // pick the same display name is not exotic - "artist" and an unconfigured default
    // are the common cases.
    // IDENTITY IS THE PROVEN KEY WHEN THERE IS ONE.
    //
    // The name-based path below is what a peer gets on a transport that cannot
    // authenticate (the loopback, an unsecured LAN socket). On an authenticated
    // transport it is not merely inferior, it is wrong: MsgHello.displayName is a string
    // the peer chose, so anyone who typed a colleague's name while that colleague was
    // offline INHERITED their UserId, and with it every lock ReclaimForUser hands back
    // and every future commit attributed to them. The transport is the only party that
    // watched a signature over a fresh nonce, so it is the only party that knows.
    std::array<u8, 64> proven{};
    if (transport_ && transport_->PeerKey(p.conn, proven)) {
        p.key = proven;
        p.keyed = true;
        const auto byKey = keyUsers_.find(proven);
        bool keyOnline = false;
        if (byKey != keyUsers_.end()) {
            for (const auto& [c2, p2] : peers_)
                if (p2.user == byKey->second && p2.helloed && &p2 != &p) {
                    keyOnline = true;
                    break;
                }
        }
        // A second LIVE connection from the same key is a second seat, not a resume -
        // otherwise one person on two machines would steal their own locks back and
        // forth. A key that is NOT online is a genuine reconnect.
        if (byKey != keyUsers_.end() && !keyOnline) p.user = byKey->second;
        else p.user = nextUser_++;
        keyUsers_[proven] = p.user;
        p.session = nextSession_++;
        FinishHello(p);
        return;
    }

    const auto known = knownUsers_.find(p.name);
    bool nameOnline = false;
    if (known != knownUsers_.end()) {
        for (const auto& [c2, p2] : peers_)
            if (p2.user == known->second && p2.helloed && &p2 != &p) { nameOnline = true; break; }
    }
    if (known != knownUsers_.end() && !nameOnline) {
        p.user = known->second; // a genuine reconnect: same person, new session
    } else {
        p.user = nextUser_++;   // new person, or the name is already online
        if (nameOnline) {
            // Disambiguate so the two are distinguishable in every UI that shows a
            // holder. Silently sharing a label is how a lock badge lies.
            p.name += " (" + std::to_string(p.user) + ")";
        }
    }
    knownUsers_[p.name] = p.user;
    p.session = nextSession_++;
    FinishHello(p);
}

// The half of OnHello that is the same however identity was decided: welcome, reclaim
// locks, announce. Shared so the key path and the name path cannot drift.
void CollabServer::FinishHello(Peer& p) {
    const u64 nowMs = lastTickMs_;

    if (writer_ == 0) writer_ = p.user;

    MsgWelcome w;
    w.user = p.user;
    w.session = p.session;
    w.isWriter = (writer_ == p.user);
    std::vector<u8> bytes;
    EncodeWelcome(bytes, w);
    SendTo(p, bytes);

    // Adopt whatever this user's previous session still holds.
    std::vector<LockView> reclaimed;
    const auto sessionLive = [this](SessionId s) {
        for (const auto& [c, pp] : peers_)
            if (pp.session == s && pp.helloed) return true;
        return false;
    };
    locks_.ReclaimForUser(p.user, p.session, nowMs, sessionLive, reclaimed);
    for (const LockView& v : reclaimed) {
        MsgLockGrant g;
        g.view = v;
        std::vector<u8> gb;
        EncodeLockGrant(gb, g);
        Broadcast(gb);
    }

    MsgPeerJoined pj;
    pj.user = p.user;
    pj.displayName = p.name;
    std::vector<u8> pjb;
    EncodePeerJoined(pjb, pj);
    Broadcast(pjb, p.conn);
}

void CollabServer::OnLockRequest(Peer& p, const MsgLockRequest& m, u64 nowMs) {
    if (p.user == 0) return;
    const Reject r = locks_.TryAcquire(m.key, p.user, p.session, nowMs);
    if (r != Reject::None) {
        MsgLockDenied d;
        d.key = m.key;
        d.reason = r;
        d.heldBy = locks_.ViewOf(m.key).owner;
        std::vector<u8> bytes;
        EncodeLockDenied(bytes, d);
        SendTo(p, bytes);
        return;
    }
    // Broadcast to EVERYONE including the requester: the requester needs the grant and
    // every peer needs the "held by" badge. One message, one code path, no divergence
    // between what the owner believes and what the others see.
    MsgLockGrant g;
    g.view = locks_.ViewOf(m.key);
    std::vector<u8> bytes;
    EncodeLockGrant(bytes, g);
    Broadcast(bytes);
}

void CollabServer::OnLockRelease(Peer& p, const MsgLockRelease& m) {
    if (p.user == 0) return;
    if (locks_.Release(m.key, p.user, p.session) != Reject::None) return;
    MsgLockGrant g;
    g.view = locks_.ViewOf(m.key);
    std::vector<u8> bytes;
    EncodeLockGrant(bytes, g);
    Broadcast(bytes);
}

// --- serving the project ------------------------------------------------------

void CollabServer::ShareProject(const std::string& root, MsgSyncManifest manifest) {
    shareRoot_ = root;
    shareManifest_ = std::move(manifest);
}

const SyncEntry* CollabServer::SharedEntry(const std::string& path) const {
    for (const SyncEntry& e : shareManifest_.files)
        if (e.path == path) return &e;
    return nullptr;
}

void CollabServer::OnSyncRequest(Peer& p) {
    if (p.user == 0) return; // not authenticated as a session yet
    // An empty manifest is a real answer: "I am sharing nothing." The alternative,
    // silence, is indistinguishable from a dropped message. But COUNT it - the asker
    // treats this as a finished copy and goes quiet, so without this nobody on either
    // machine ever learns that a transfer completed with no files in it.
    if (!Sharing()) ++asksWithNothingToSend_;
    std::vector<u8> bytes;
    EncodeSyncManifest(bytes, shareManifest_);
    SendTo(p, bytes);
}

void CollabServer::OnFileRequest(Peer& p, const MsgFileRequest& m) {
    if (p.user == 0) return;
    // THE MANIFEST IS THE ALLOWLIST. Anything else - "../../../etc/passwd", an absolute
    // path, a file that merely exists on this disk - is simply not in it and is refused
    // without ever touching the filesystem.
    if (!SharedEntry(m.path)) return;
    p.sendPath = m.path;
    p.sendOffset = 0;
}

void CollabServer::PumpFileSends() {
    if (shareRoot_.empty()) return;
    for (auto& [conn, p] : peers_) {
        if (p.sendPath.empty()) continue;
        const SyncEntry* want = SharedEntry(p.sendPath);
        if (!want) {
            p.sendPath.clear();
            continue;
        }
        std::ifstream in(shareRoot_ + "/" + p.sendPath, std::ios::binary);
        if (!in) {
            // The file went away since the manifest was built. Say so by sending a final
            // empty chunk rather than going quiet - the receiver is waiting on this file
            // and would otherwise hang for the rest of the session.
            MsgFileChunk c;
            c.path = p.sendPath;
            c.offset = p.sendOffset;
            c.last = true;
            std::vector<u8> bytes;
            EncodeFileChunk(bytes, c);
            SendTo(p, bytes);
            p.sendPath.clear();
            continue;
        }
        in.seekg(static_cast<std::streamoff>(p.sendOffset));
        std::vector<u8> buf(kFileChunkBytes);
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const usize got = static_cast<usize>(in.gcount());
        buf.resize(got);

        MsgFileChunk c;
        c.path = p.sendPath;
        c.offset = p.sendOffset;
        c.data = std::move(buf);
        p.sendOffset += got;
        c.last = p.sendOffset >= want->size || got == 0;
        std::vector<u8> bytes;
        EncodeFileChunk(bytes, c);
        SendTo(p, bytes);
        if (c.last) {
            p.sendPath.clear();
            p.sendOffset = 0;
        }
    }
}

void CollabServer::LocalPaint(CanvasId canvas, u32 layerId,
                              const std::vector<u8>& strokeBlob) {
    MsgPaintCommitted c;
    c.canvas = canvas;
    c.layerId = layerId;
    c.strokeBlob = strokeBlob;
    c.seq = ++seq_;
    c.author = kLocalUser;
    paintLog_[canvas].push_back(c);
    std::vector<u8> bytes;
    EncodePaintCommitted(bytes, c);
    // To the guests. The host already has this stroke on its own canvas; handing it back
    // would stamp it twice.
    Broadcast(bytes);
}

// --- the host's own edits -----------------------------------------------------

bool CollabServer::LocalLock(const EntityKey& k, u64 nowMs) {
    return locks_.TryAcquire(k, kLocalUser, kLocalSession, nowMs) == Reject::None;
}

void CollabServer::LocalUnlock(const EntityKey& k) {
    locks_.Release(k, kLocalUser, kLocalSession);
}

bool CollabServer::LocalHoldsLock(const EntityKey& k) const {
    return locks_.HoldsLock(k, kLocalUser, kLocalSession);
}

bool CollabServer::LocalDelta(const EntityKey& k, const std::string& componentKey,
                              const std::string& json) {
    // THE SAME GATE THE WIRE PATH USES. The host is closer to the authority, not exempt
    // from it: without this it could overwrite an entity a guest is holding, and the
    // guest's editor would show the change with no indication anyone had taken it.
    if (!locks_.HoldsLock(k, kLocalUser, kLocalSession)) return false;

    if (json.empty()) state_[k].erase(componentKey);
    else state_[k][componentKey] = json;

    MsgDeltaApplied a;
    a.key = k;
    a.componentKey = componentKey;
    a.json = json;
    a.revision = locks_.BumpRevision(k);
    a.seq = ++seq_;
    a.author = kLocalUser;
    std::vector<u8> bytes;
    EncodeDeltaApplied(bytes, a);
    // To the guests only - the host already has this change in its own registry, and
    // handing it back would make the editor re-apply what the author is still dragging.
    Broadcast(bytes);
    return true;
}

void CollabServer::OnEntityLife(Peer& p, const MsgEntityLife& m) {
    if (p.user == 0) return;
    // DESTROY NEEDS THE LOCK. Creating does not - there is no entity to hold yet, and
    // requiring a lock for it would mean asking permission for something nobody else
    // knows about. Deleting is the opposite: an object somebody else is holding must not
    // vanish under them.
    if (m.destroy && !locks_.HoldsLock(m.key, p.user, p.session)) return;
    if (m.destroy) state_.erase(m.key);

    MsgEntityLived a;
    a.key = m.key;
    a.destroy = m.destroy;
    a.name = m.name;
    a.seq = ++seq_;
    a.author = p.user;
    std::vector<u8> bytes;
    EncodeEntityLived(bytes, a);
    Broadcast(bytes);
    if (onLived) onLived(a);
}

bool CollabServer::LocalLife(const EntityKey& k, bool destroy, const std::string& name) {
    if (destroy && !locks_.HoldsLock(k, kLocalUser, kLocalSession)) return false;
    if (destroy) state_.erase(k);
    MsgEntityLived a;
    a.key = k;
    a.destroy = destroy;
    a.name = name;
    a.seq = ++seq_;
    a.author = kLocalUser;
    std::vector<u8> bytes;
    EncodeEntityLived(bytes, a);
    Broadcast(bytes);
    return true;
}

void CollabServer::OnEntityDelta(Peer& p, const MsgEntityDelta& m) {
    const auto deny = [&](Reject why) {
        MsgDeltaRejected d;
        d.key = m.key;
        d.componentKey = m.componentKey;
        d.reason = why;
        d.currentRevision = locks_.RevisionOf(m.key);
        std::vector<u8> bytes;
        EncodeDeltaRejected(bytes, d);
        SendTo(p, bytes);
    };

    if (p.user == 0) return deny(Reject::NotConnected);
    // THE LOCK IS THE WRITE PERMISSION. Checked before the revision so a user editing
    // something they do not own is told THAT, rather than being sent away to re-base
    // an edit that would be refused anyway.
    if (!locks_.HoldsLock(m.key, p.user, p.session)) return deny(Reject::NotOwner);
    // NO STALE-REVISION REJECTION FOR THE LOCK OWNER, AND THAT IS THE WHOLE POINT OF
    // HOLDING A LOCK.
    //
    // This used to be `if (m.baseRevision != cur) return deny(Reject::StaleRevision);`,
    // and it SILENTLY DESTROYED WORK. The lock is exclusive, so nobody else can have
    // written between the client's read and its write; the only revisions in that gap
    // are the client's OWN earlier edits, still in flight toward it. So changing two
    // components of one entity - a gizmo drag plus a rename, an inspector row that
    // touches a component pair, anything at all in the same round trip - had its second
    // edit rejected. And MsgDeltaRejected does not carry the json while CollabClient
    // keeps no copy, so that edit was not retried or reported: it was simply gone, on a
    // path with no test covering it.
    //
    // The revision remains a VERSION NUMBER - bumped, broadcast, and used by clients to
    // order what they receive. It is not a permission. The permission is the lock, which
    // was already checked above.
    (void)m.baseRevision;

    if (m.json.empty()) state_[m.key].erase(m.componentKey);
    else state_[m.key][m.componentKey] = m.json;

    MsgDeltaApplied a;
    a.key = m.key;
    a.componentKey = m.componentKey;
    a.json = m.json;
    a.revision = locks_.BumpRevision(m.key);
    a.seq = ++seq_;
    a.author = p.user;
    std::vector<u8> bytes;
    EncodeDeltaApplied(bytes, a);
    // Including the author: they need the server's revision to base their NEXT edit
    // on. A client that assumed its own local revision would desync on its second edit.
    Broadcast(bytes);
    // The host's own scene is not a peer and receives no broadcast, so this is the only
    // thing that tells it a guest moved something.
    if (onApplied) onApplied(a);
}

void CollabServer::OnPaintOp(Peer& p, const MsgPaintOp& m) {
    if (p.user == 0 || m.canvas == 0) return;
    // Painting is NOT gated on an entity lock. Two artists on one canvas is the
    // headline feature, and the op log makes it safe: every stroke is ordered, so the
    // result is deterministic even when they overlap. Locking would forbid the thing
    // the feature exists to allow.
    MsgPaintCommitted c;
    c.canvas = m.canvas;
    c.layerId = m.layerId;
    c.strokeBlob = m.strokeBlob;
    c.seq = ++seq_;
    c.author = p.user;
    paintLog_[m.canvas].push_back(c);
    std::vector<u8> bytes;
    EncodePaintCommitted(bytes, c);
    Broadcast(bytes);
    // The host's own canvas is the one thing a broadcast does not reach.
    if (onPainted) onPainted(c);
}

void CollabServer::OnPaintPreview(Peer& p, const MsgPaintPreview& m) {
    if (p.user == 0 || m.canvas == 0) return;
    // RELAYED, NEVER LOGGED. A preview that reached the durable history would be
    // indistinguishable from a committed stroke on replay, and the canvas would grow
    // pixels the artist never committed. It is a different message type precisely so
    // this function cannot reach paintLog_ by accident.
    std::vector<u8> bytes;
    EncodePaintPreview(bytes, m);
    Broadcast(bytes, p.conn);
}

void CollabServer::SendTo(Peer& p, const std::vector<u8>& bytes) {
    if (transport_ && !bytes.empty()) transport_->Send(p.conn, bytes.data(), bytes.size());
}

void CollabServer::Broadcast(const std::vector<u8>& bytes, ConnId except) {
    if (!transport_ || bytes.empty()) return;
    for (auto& [conn, p] : peers_) {
        if (conn == except) continue;
        transport_->Send(conn, bytes.data(), bytes.size());
    }
}

const std::string* CollabServer::ComponentState(const EntityKey& k,
                                                const std::string& comp) const {
    const auto e = state_.find(k);
    if (e == state_.end()) return nullptr;
    const auto c = e->second.find(comp);
    return c == e->second.end() ? nullptr : &c->second;
}

const std::vector<MsgPaintCommitted>& CollabServer::PaintHistory(CanvasId c) const {
    const auto it = paintLog_.find(c);
    return it == paintLog_.end() ? kNoHistory : it->second;
}

} // namespace hbe::collab
