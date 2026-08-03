// Collab/Protocol.cpp - all encode/decode, deliberately in one file.
//
// Keeping every wire struct's read and write side adjacent is what stops them
// drifting: a field added to one and forgotten in the other is visible in the diff.
#include "Collab/Protocol.h"

#include <cstring>

namespace hbe::collab {

const char* RejectName(Reject r) {
    switch (r) {
        case Reject::None: return "None";
        case Reject::NotConnected: return "NotConnected";
        case Reject::HeldByOther: return "HeldByOther";
        case Reject::NotOwner: return "NotOwner";
        case Reject::StaleRevision: return "StaleRevision";
        case Reject::UnknownEntity: return "UnknownEntity";
        case Reject::ReadOnlySession: return "ReadOnlySession";
        case Reject::Malformed: return "Malformed";
        case Reject::Count: break;
    }
    return "?";
}

const char* MsgTypeName(MsgType t) {
    switch (t) {
        case MsgType::Invalid: return "Invalid";
        case MsgType::Hello: return "Hello";
        case MsgType::JoinDoc: return "JoinDoc";
        case MsgType::LockRequest: return "LockRequest";
        case MsgType::LockRelease: return "LockRelease";
        case MsgType::Heartbeat: return "Heartbeat";
        case MsgType::EntityDelta: return "EntityDelta";
        case MsgType::PaintOp: return "PaintOp";
        case MsgType::PaintPreview: return "PaintPreview";
        case MsgType::Welcome: return "Welcome";
        case MsgType::LockGrant: return "LockGrant";
        case MsgType::LockDenied: return "LockDenied";
        case MsgType::DeltaApplied: return "DeltaApplied";
        case MsgType::DeltaRejected: return "DeltaRejected";
        case MsgType::PaintCommitted: return "PaintCommitted";
        case MsgType::PeerJoined: return "PeerJoined";
        case MsgType::PeerLeft: return "PeerLeft";
        case MsgType::SyncRequest: return "SyncRequest";
        case MsgType::SyncManifest: return "SyncManifest";
        case MsgType::FileRequest: return "FileRequest";
        case MsgType::FileChunk: return "FileChunk";
        case MsgType::EntityLife: return "EntityLife";
        case MsgType::EntityLived: return "EntityLived";
        case MsgType::Count: break;
    }
    return "?";
}

namespace {

// Frames a BinaryWriter's payload into `out`. The header is written last (once the
// payload length is known) but placed first, so the stream stays self-describing.
void Emit(std::vector<u8>& out, MsgType type, const BinaryWriter& w) {
    const std::vector<u8>& body = w.Data();
    // REFUSE TO ENCODE what our own decoder is required to reject. Without this the
    // encoder can emit a frame larger than kMaxFramePayload, which the peer treats as
    // an unrecoverable header and drops the connection for - so an honest oversized
    // message (a huge component JSON, a giant stroke blob) presents as a mysterious
    // disconnect. Dropping it here is visible and local instead.
    if (body.size() > kMaxFramePayload) return;
    FrameHeader h;
    h.length = static_cast<u32>(body.size());
    h.kind = static_cast<u16>(type);
    h.version = kProtocolVersion;
    const usize base = out.size();
    out.resize(base + kFrameHeaderBytes + body.size());
    std::memcpy(out.data() + base, &h.length, sizeof(u32));
    std::memcpy(out.data() + base + 4, &h.kind, sizeof(u16));
    std::memcpy(out.data() + base + 6, &h.version, sizeof(u16));
    if (!body.empty())
        std::memcpy(out.data() + base + kFrameHeaderBytes, body.data(), body.size());
}

void PutKey(BinaryWriter& w, const EntityKey& k) {
    w.Pod(k.doc);
    w.Pod(k.guid);
}
bool GetKey(BinaryReader& r, EntityKey& k) { return r.Pod(k.doc) && r.Pod(k.guid); }

void PutView(BinaryWriter& w, const LockView& v) {
    PutKey(w, v.key);
    w.Pod(v.owner);
    w.Pod(v.session);
    w.Pod(v.revision);
}
bool GetView(BinaryReader& r, LockView& v) {
    return GetKey(r, v.key) && r.Pod(v.owner) && r.Pod(v.session) && r.Pod(v.revision);
}

} // namespace

// --- encode ------------------------------------------------------------------

void EncodeHello(std::vector<u8>& out, const MsgHello& m) {
    BinaryWriter w;
    w.Str(m.displayName);
    w.Pod(m.resumeUser);
    Emit(out, MsgType::Hello, w);
}
void EncodeWelcome(std::vector<u8>& out, const MsgWelcome& m) {
    BinaryWriter w;
    w.Pod(m.user);
    w.Pod(m.session);
    w.Pod(static_cast<u8>(m.isWriter ? 1 : 0));
    w.Pod(m.serverProtocol);
    Emit(out, MsgType::Welcome, w);
}
void EncodeJoinDoc(std::vector<u8>& out, const MsgJoinDoc& m) {
    BinaryWriter w;
    w.Pod(m.doc);
    Emit(out, MsgType::JoinDoc, w);
}
void EncodeLockRequest(std::vector<u8>& out, const MsgLockRequest& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    Emit(out, MsgType::LockRequest, w);
}
void EncodeLockRelease(std::vector<u8>& out, const MsgLockRelease& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    Emit(out, MsgType::LockRelease, w);
}
void EncodeHeartbeat(std::vector<u8>& out, const MsgHeartbeat& m) {
    BinaryWriter w;
    w.Pod(m.nowMsHint);
    Emit(out, MsgType::Heartbeat, w);
}
void EncodeEntityDelta(std::vector<u8>& out, const MsgEntityDelta& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    w.Str(m.componentKey);
    w.Str(m.json);
    w.Pod(m.baseRevision);
    Emit(out, MsgType::EntityDelta, w);
}
void EncodeDeltaApplied(std::vector<u8>& out, const MsgDeltaApplied& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    w.Str(m.componentKey);
    w.Str(m.json);
    w.Pod(m.revision);
    w.Pod(m.seq);
    w.Pod(m.author);
    Emit(out, MsgType::DeltaApplied, w);
}
void EncodeDeltaRejected(std::vector<u8>& out, const MsgDeltaRejected& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    w.Str(m.componentKey);
    w.Pod(static_cast<u8>(m.reason));
    w.Pod(m.currentRevision);
    Emit(out, MsgType::DeltaRejected, w);
}
void EncodeEntityLife(std::vector<u8>& out, const MsgEntityLife& m) {
    BinaryWriter w;
    w.Pod(m.key.doc);
    w.Pod(m.key.guid);
    w.Pod(static_cast<u8>(m.destroy ? 1 : 0));
    w.Str(m.name);
    Emit(out, MsgType::EntityLife, w);
}

void EncodeEntityLived(std::vector<u8>& out, const MsgEntityLived& m) {
    BinaryWriter w;
    w.Pod(m.key.doc);
    w.Pod(m.key.guid);
    w.Pod(static_cast<u8>(m.destroy ? 1 : 0));
    w.Str(m.name);
    w.Pod(m.seq);
    w.Pod(m.author);
    Emit(out, MsgType::EntityLived, w);
}

std::optional<MsgEntityLife> DecodeEntityLife(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgEntityLife m;
    r.Pod(m.key.doc);
    r.Pod(m.key.guid);
    u8 d = 0;
    r.Pod(d);
    r.Str(m.name);
    if (!r.Ok()) return std::nullopt;
    m.destroy = d != 0;
    return m;
}

std::optional<MsgEntityLived> DecodeEntityLived(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgEntityLived m;
    r.Pod(m.key.doc);
    r.Pod(m.key.guid);
    u8 d = 0;
    r.Pod(d);
    r.Str(m.name);
    r.Pod(m.seq);
    r.Pod(m.author);
    if (!r.Ok()) return std::nullopt;
    m.destroy = d != 0;
    return m;
}

// --- project transfer ---------------------------------------------------------

void EncodeSyncRequest(std::vector<u8>& out, const MsgSyncRequest& m) {
    BinaryWriter w;
    w.Pod(m.unused);
    Emit(out, MsgType::SyncRequest, w);
}

void EncodeSyncManifest(std::vector<u8>& out, const MsgSyncManifest& m) {
    BinaryWriter w;
    w.Pod(static_cast<u32>(m.files.size()));
    for (const SyncEntry& e : m.files) {
        w.Str(e.path);
        w.Pod(e.size);
        w.Str(e.sha256);
    }
    Emit(out, MsgType::SyncManifest, w);
}

void EncodeFileRequest(std::vector<u8>& out, const MsgFileRequest& m) {
    BinaryWriter w;
    w.Str(m.path);
    Emit(out, MsgType::FileRequest, w);
}

void EncodeFileChunk(std::vector<u8>& out, const MsgFileChunk& m) {
    BinaryWriter w;
    w.Str(m.path);
    w.Pod(m.offset);
    w.Vec(m.data);
    w.Pod(static_cast<u8>(m.last ? 1 : 0));
    Emit(out, MsgType::FileChunk, w);
}

std::optional<MsgSyncRequest> DecodeSyncRequest(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgSyncRequest m;
    r.Pod(m.unused);
    if (!r.Ok()) return std::nullopt;
    return m;
}

std::optional<MsgSyncManifest> DecodeSyncManifest(const u8* p, usize n) {
    BinaryReader r(p, n);
    u32 count = 0;
    r.Pod(count);
    if (!r.Ok()) return std::nullopt;
    // BOUND BEFORE RESERVING. A count field is attacker-controlled; reserving on it
    // first is how a four-byte message becomes an out-of-memory crash.
    if (count > kMaxSyncFiles) return std::nullopt;
    MsgSyncManifest m;
    m.files.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        SyncEntry e;
        r.Str(e.path);
        r.Pod(e.size);
        r.Str(e.sha256);
        if (!r.Ok()) return std::nullopt;
        m.files.push_back(std::move(e));
    }
    return m;
}

std::optional<MsgFileRequest> DecodeFileRequest(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgFileRequest m;
    r.Str(m.path);
    if (!r.Ok()) return std::nullopt;
    return m;
}

std::optional<MsgFileChunk> DecodeFileChunk(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgFileChunk m;
    r.Str(m.path);
    r.Pod(m.offset);
    r.Vec(m.data);
    u8 last = 0;
    r.Pod(last);
    if (!r.Ok()) return std::nullopt;
    m.last = last != 0;
    return m;
}

void EncodePaintOp(std::vector<u8>& out, const MsgPaintOp& m) {
    BinaryWriter w;
    w.Pod(m.canvas);
    w.Pod(m.layerId);
    w.Vec(m.strokeBlob);
    Emit(out, MsgType::PaintOp, w);
}
void EncodePaintCommitted(std::vector<u8>& out, const MsgPaintCommitted& m) {
    BinaryWriter w;
    w.Pod(m.canvas);
    w.Pod(m.layerId);
    w.Vec(m.strokeBlob);
    w.Pod(m.seq);
    w.Pod(m.author);
    Emit(out, MsgType::PaintCommitted, w);
}
void EncodePaintPreview(std::vector<u8>& out, const MsgPaintPreview& m) {
    BinaryWriter w;
    w.Pod(m.canvas);
    w.Pod(m.layerId);
    w.Vec(m.partialBlob);
    Emit(out, MsgType::PaintPreview, w);
}
void EncodeLockGrant(std::vector<u8>& out, const MsgLockGrant& m) {
    BinaryWriter w;
    PutView(w, m.view);
    w.Pod(static_cast<u8>(m.expired ? 1 : 0));
    Emit(out, MsgType::LockGrant, w);
}
void EncodeLockDenied(std::vector<u8>& out, const MsgLockDenied& m) {
    BinaryWriter w;
    PutKey(w, m.key);
    w.Pod(static_cast<u8>(m.reason));
    w.Pod(m.heldBy);
    Emit(out, MsgType::LockDenied, w);
}
void EncodePeerJoined(std::vector<u8>& out, const MsgPeerJoined& m) {
    BinaryWriter w;
    w.Pod(m.user);
    w.Str(m.displayName);
    Emit(out, MsgType::PeerJoined, w);
}
void EncodePeerLeft(std::vector<u8>& out, const MsgPeerLeft& m) {
    BinaryWriter w;
    w.Pod(m.user);
    Emit(out, MsgType::PeerLeft, w);
}

// --- decode ------------------------------------------------------------------
//
// Every one of these returns nullopt rather than a partly-filled struct when the
// bytes run out: BinaryReader latches its error flag, so one Ok() check at the end
// covers every field above it.

std::optional<MsgHello> DecodeHello(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgHello m;
    r.Str(m.displayName);
    r.Pod(m.resumeUser);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgWelcome> DecodeWelcome(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgWelcome m;
    u8 writer = 0;
    r.Pod(m.user);
    r.Pod(m.session);
    r.Pod(writer);
    r.Pod(m.serverProtocol);
    if (!r.Ok()) return std::nullopt;
    m.isWriter = writer != 0;
    return m;
}
std::optional<MsgJoinDoc> DecodeJoinDoc(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgJoinDoc m;
    r.Pod(m.doc);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgLockRequest> DecodeLockRequest(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgLockRequest m;
    GetKey(r, m.key);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgLockRelease> DecodeLockRelease(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgLockRelease m;
    GetKey(r, m.key);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgHeartbeat> DecodeHeartbeat(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgHeartbeat m;
    r.Pod(m.nowMsHint);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgEntityDelta> DecodeEntityDelta(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgEntityDelta m;
    GetKey(r, m.key);
    r.Str(m.componentKey);
    r.Str(m.json);
    r.Pod(m.baseRevision);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgDeltaApplied> DecodeDeltaApplied(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgDeltaApplied m;
    GetKey(r, m.key);
    r.Str(m.componentKey);
    r.Str(m.json);
    r.Pod(m.revision);
    r.Pod(m.seq);
    r.Pod(m.author);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgDeltaRejected> DecodeDeltaRejected(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgDeltaRejected m;
    u8 reason = 0;
    GetKey(r, m.key);
    r.Str(m.componentKey);
    r.Pod(reason);
    r.Pod(m.currentRevision);
    if (!r.Ok() || reason >= static_cast<u8>(Reject::Count)) return std::nullopt;
    m.reason = static_cast<Reject>(reason);
    return m;
}
std::optional<MsgPaintOp> DecodePaintOp(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgPaintOp m;
    r.Pod(m.canvas);
    r.Pod(m.layerId);
    r.Vec(m.strokeBlob);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgPaintCommitted> DecodePaintCommitted(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgPaintCommitted m;
    r.Pod(m.canvas);
    r.Pod(m.layerId);
    r.Vec(m.strokeBlob);
    r.Pod(m.seq);
    r.Pod(m.author);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgPaintPreview> DecodePaintPreview(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgPaintPreview m;
    r.Pod(m.canvas);
    r.Pod(m.layerId);
    r.Vec(m.partialBlob);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgLockGrant> DecodeLockGrant(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgLockGrant m;
    u8 expired = 0;
    GetView(r, m.view);
    r.Pod(expired);
    if (!r.Ok()) return std::nullopt;
    m.expired = expired != 0;
    return m;
}
std::optional<MsgLockDenied> DecodeLockDenied(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgLockDenied m;
    u8 reason = 0;
    GetKey(r, m.key);
    r.Pod(reason);
    r.Pod(m.heldBy);
    if (!r.Ok() || reason >= static_cast<u8>(Reject::Count)) return std::nullopt;
    m.reason = static_cast<Reject>(reason);
    return m;
}
std::optional<MsgPeerJoined> DecodePeerJoined(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgPeerJoined m;
    r.Pod(m.user);
    r.Str(m.displayName);
    if (!r.Ok()) return std::nullopt;
    return m;
}
std::optional<MsgPeerLeft> DecodePeerLeft(const u8* p, usize n) {
    BinaryReader r(p, n);
    MsgPeerLeft m;
    r.Pod(m.user);
    if (!r.Ok()) return std::nullopt;
    return m;
}

// --- framing -----------------------------------------------------------------

FrameScan PeekFrame(const u8* p, usize available, FrameHeader& out) {
    if (available < kFrameHeaderBytes) return FrameScan::Incomplete;
    std::memcpy(&out.length, p, sizeof(u32));
    std::memcpy(&out.kind, p + 4, sizeof(u16));
    std::memcpy(&out.version, p + 6, sizeof(u16));
    // Bound BEFORE the caller allocates anything, and report it as INVALID rather than
    // "wait for more". A hostile length can never become legal by waiting, so treating
    // it as a partial tail wedges the connection and grows its buffer without bound.
    if (out.length > kMaxFramePayload) return FrameScan::Invalid;
    // usize is 64-bit here and length is bounded above, so this cannot overflow.
    return available >= kFrameHeaderBytes + static_cast<usize>(out.length)
               ? FrameScan::Ok
               : FrameScan::Incomplete;
}

std::vector<Frame> SplitFrames(const u8* p, usize n, usize& consumed, bool& fatal) {
    std::vector<Frame> frames;
    consumed = 0;
    fatal = false;
    while (consumed < n) {
        FrameHeader h;
        const FrameScan scan = PeekFrame(p + consumed, n - consumed, h);
        if (scan == FrameScan::Invalid) {
            // Unrecoverable: a length-prefixed stream cannot resynchronise once a
            // length is untrustworthy. Report it and stop; the caller drops the peer.
            fatal = true;
            break;
        }
        if (scan == FrameScan::Incomplete) break; // partial tail: keep it
        Frame f;
        f.type = h.kind < static_cast<u16>(MsgType::Count) ? static_cast<MsgType>(h.kind)
                                                           : MsgType::Invalid;
        f.version = h.version;
        f.payload = p + consumed + kFrameHeaderBytes;
        f.size = h.length;
        frames.push_back(f);
        consumed += kFrameHeaderBytes + h.length;
        // An UNKNOWN kind is still consumed, not fatal. That is the whole point of the
        // length field: a newer peer can send something this build has never heard of
        // and the stream stays in sync. The dispatcher sees MsgType::Invalid and skips.
    }
    return frames;
}

} // namespace hbe::collab
