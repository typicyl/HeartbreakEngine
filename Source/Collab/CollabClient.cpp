// Collab/CollabClient.cpp
#include "Collab/CollabClient.h"

namespace hbe::collab {

void CollabClient::Hello(const std::string& displayName, UserId resumeUser) {
    MsgHello m;
    m.displayName = displayName;
    m.resumeUser = resumeUser;
    EncodeHello(outbox_, m);
    Flush();
}

void CollabClient::JoinDocument(DocId doc) {
    EncodeJoinDoc(outbox_, MsgJoinDoc{doc});
    Flush();
}

void CollabClient::Pump(u64 nowMs) {
    if (!transport_) return;

    // HEARTBEAT FIRST, and unconditionally while connected. Sending it only when locks
    // are held would drop every lease the instant an artist deselects - and they would
    // then have to re-acquire on the next click, racing anyone else who clicked first.
    if (user_ != 0 && (nowMs - lastHeartbeatMs_) >= kLockHeartbeatMs) {
        lastHeartbeatMs_ = nowMs;
        MsgHeartbeat hb;
        hb.nowMsHint = static_cast<u32>(nowMs);
        EncodeHeartbeat(outbox_, hb);
    }
    Flush();

    transport_->Receive(inbox_);
    if (inbox_.empty()) return;
    usize consumed = 0;
    bool fatal = false;
    const std::vector<Frame> frames = SplitFrames(inbox_.data(), inbox_.size(), consumed, fatal);
    for (const Frame& f : frames) Dispatch(f);
    if (fatal && transport_) {
        // Same reasoning as the server: an untrustworthy length cannot be resynchronised.
        transport_->Disconnect();
        inbox_.clear();
        return;
    }
    // Keep the partial tail. Erasing the whole buffer would drop the front of a frame
    // that has not fully arrived, and every subsequent byte would be misinterpreted.
    inbox_.erase(inbox_.begin(), inbox_.begin() + static_cast<std::ptrdiff_t>(consumed));
}

void CollabClient::Dispatch(const Frame& f) {
    // See CollabServer::HandleFrame - a known kind at an unknown version decodes into
    // plausible garbage rather than failing, so it must be skipped, not parsed.
    if (f.version != kProtocolVersion) return;
    switch (f.type) {
        case MsgType::Welcome:
            if (const auto m = DecodeWelcome(f.payload, f.size)) {
                user_ = m->user;
                session_ = m->session;
                isWriter_ = m->isWriter;
            }
            break;
        case MsgType::LockGrant:
            if (const auto m = DecodeLockGrant(f.payload, f.size)) {
                Known& k = known_[m->view.key];
                k.owner = m->view.owner;
                k.revision = m->view.revision;
                if (cb_.onLock) cb_.onLock(*m);
            }
            break;
        case MsgType::LockDenied:
            if (const auto m = DecodeLockDenied(f.payload, f.size)) {
                known_[m->key].owner = m->heldBy;
                if (cb_.onLockDenied) cb_.onLockDenied(*m);
            }
            break;
        case MsgType::DeltaApplied:
            if (const auto m = DecodeDeltaApplied(f.payload, f.size)) {
                // Track the server's revision even for our OWN accepted edit - that is
                // what the next edit must be based on.
                known_[m->key].revision = m->revision;
                if (cb_.onDelta) cb_.onDelta(*m);
            }
            break;
        case MsgType::DeltaRejected:
            if (const auto m = DecodeDeltaRejected(f.payload, f.size)) {
                known_[m->key].revision = m->currentRevision; // re-base
                if (cb_.onRejected) cb_.onRejected(*m);
            }
            break;
        case MsgType::PaintCommitted:
            if (const auto m = DecodePaintCommitted(f.payload, f.size))
                if (cb_.onPaint) cb_.onPaint(*m);
            break;
        case MsgType::PaintPreview:
            if (const auto m = DecodePaintPreview(f.payload, f.size))
                if (cb_.onPaintPreview) cb_.onPaintPreview(*m);
            break;
        case MsgType::PeerJoined:
            if (const auto m = DecodePeerJoined(f.payload, f.size))
                if (cb_.onPeerJoined) cb_.onPeerJoined(*m);
            break;
        case MsgType::SyncManifest:
            if (const auto m = DecodeSyncManifest(f.payload, f.size))
                if (cb_.onManifest) cb_.onManifest(*m);
            break;
        case MsgType::FileChunk:
            if (const auto m = DecodeFileChunk(f.payload, f.size))
                if (cb_.onFileChunk) cb_.onFileChunk(*m);
            break;
        case MsgType::PeerLeft:
            if (const auto m = DecodePeerLeft(f.payload, f.size))
                if (cb_.onPeerLeft) cb_.onPeerLeft(*m);
            break;
        default:
            break; // unknown kind: already consumed by SplitFrames, stream stays in sync
    }
}

void CollabClient::RequestLock(const EntityKey& k) {
    EncodeLockRequest(outbox_, MsgLockRequest{k});
    Flush();
}

void CollabClient::ReleaseLock(const EntityKey& k) {
    EncodeLockRelease(outbox_, MsgLockRelease{k});
    Flush();
}

void CollabClient::SendDelta(const EntityKey& k, const std::string& componentKey,
                             const std::string& json) {
    MsgEntityDelta m;
    m.key = k;
    m.componentKey = componentKey;
    m.json = json;
    m.baseRevision = KnownRevision(k);
    EncodeEntityDelta(outbox_, m);
    Flush();
}

void CollabClient::SendPaintOp(CanvasId canvas, u32 layerId,
                               const std::vector<u8>& strokeBlob) {
    MsgPaintOp m;
    m.canvas = canvas;
    m.layerId = layerId;
    m.strokeBlob = strokeBlob;
    EncodePaintOp(outbox_, m);
    Flush();
}

void CollabClient::SendPaintPreview(CanvasId canvas, u32 layerId,
                                    const std::vector<u8>& partial) {
    MsgPaintPreview m;
    m.canvas = canvas;
    m.layerId = layerId;
    m.partialBlob = partial;
    EncodePaintPreview(outbox_, m);
    Flush();
}

void CollabClient::RequestProject() {
    EncodeSyncRequest(outbox_, MsgSyncRequest{});
    Flush();
}

void CollabClient::RequestFile(const std::string& path) {
    EncodeFileRequest(outbox_, MsgFileRequest{path});
    Flush();
}

Revision CollabClient::KnownRevision(const EntityKey& k) const {
    const auto it = known_.find(k);
    return it == known_.end() ? 0u : it->second.revision;
}

UserId CollabClient::LockOwner(const EntityKey& k) const {
    const auto it = known_.find(k);
    return it == known_.end() ? 0u : it->second.owner;
}

bool CollabClient::CanEdit(const EntityKey& k) const {
    // Unlocked is NOT editable: the server refuses a delta without a lock, so letting
    // the inspector look editable would produce a rejection the artist did not cause
    // and cannot interpret. The editor requests the lock on selection instead.
    return user_ != 0 && LockOwner(k) == user_;
}

void CollabClient::Flush() {
    if (!transport_ || outbox_.empty()) return;
    transport_->Send(outbox_.data(), outbox_.size());
    outbox_.clear();
}

} // namespace hbe::collab
