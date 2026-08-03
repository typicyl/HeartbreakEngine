// Collab/Protocol.h - every byte that crosses the wire, and the durable record format.
//
// ONE RULE GOVERNS THIS FILE: every frame and every durable record begins with
// {u32 length, u16 kind, u16 version}, and a reader that does not recognise `kind`
// MUST be able to skip exactly `length` bytes and carry on.
//
// That 8-byte header is what makes every OTHER decision in this design reversible. A
// newer client can send a message an older server has never heard of; an older server
// can read a history containing records it cannot interpret and still replay the ones
// it can. Without it, the first protocol change orphans every recorded history and
// every connected peer at once - and this engine has ten hand-rolled binary formats
// that each solved framing separately, so the cost of NOT having it is well evidenced.
//
// Core/BinaryStream is already hostile-input safe (Str/Vec validate the remaining
// size before resizing and latch an `ok_` flag), but it has no framing and no
// versioning of its own. This file is that missing layer.
#pragma once

#include "Collab/CollabTypes.h"
#include "Core/BinaryStream.h"

#include <optional>
#include <string>
#include <vector>

namespace hbe::collab {

// ---------------------------------------------------------------------------
// FRAMING
// ---------------------------------------------------------------------------

enum class MsgType : u16 {
    Invalid = 0,
    // client -> server
    Hello,          // "I am <display name>", optionally resuming a previous UserId
    JoinDoc,        // start receiving a document's state and deltas
    LockRequest,    // acquire an entity lock
    LockRelease,    // give it back
    Heartbeat,      // renew every lock this session holds
    EntityDelta,    // "component C of entity E becomes this, based on revision R"
    PaintOp,        // a committed brush operation (durable)
    PaintPreview,   // an IN-PROGRESS stroke (never durable - see below)
    // server -> client
    Welcome,        // your UserId + SessionId + whether you are the writer
    LockGrant,      // lock state changed (granted, released, expired, reclaimed)
    LockDenied,     // your request lost
    DeltaApplied,   // an accepted EntityDelta, in server order, with its new revision
    DeltaRejected,  // your delta lost; carries the current revision so you can re-base
    PaintCommitted, // an accepted PaintOp, in server order
    PeerJoined,
    PeerLeft,
    // --- PROJECT TRANSFER -----------------------------------------------------
    // APPENDED, and that is not a style preference. This enum has no explicit values,
    // so an enumerator's ORDINAL is what goes on the wire; inserting one anywhere but
    // here renumbers every kind after it, and two builds claiming the same
    // kProtocolVersion would then disagree about what a frame means. Appending is also
    // why kProtocolVersion is NOT bumped: both dispatchers drop a frame whose version
    // differs, so a bump severs all compatibility in both directions at once, whereas an
    // unknown kind is already skipped harmlessly.
    SyncRequest,  // client -> server: "what files do you have?"
    SyncManifest, // server -> client: the list, with a hash per file
    FileRequest,  // client -> server: "send me this one"
    FileChunk,    // server -> client: bytes, in order, with a final marker
    EntityLife,   // client -> server: an entity was CREATED or DESTROYED
    EntityLived,  // server -> client: the accepted creation/destruction, in order
    Count
};
const char* MsgTypeName(MsgType t);

// Every frame starts with this. `length` counts the PAYLOAD ONLY (the bytes after
// this header), so a skipper does not have to know whether the header is included.
struct FrameHeader {
    u32 length = 0;
    u16 kind = static_cast<u16>(MsgType::Invalid);
    u16 version = kProtocolVersion;
};
inline constexpr usize kFrameHeaderBytes = 8;

// A frame cap the reader enforces BEFORE allocating. Without it, a corrupt or hostile
// `length` is an instant multi-gigabyte allocation. 16 MiB is far above any legitimate
// message (the largest is a component's JSON) and far below a denial of service.
inline constexpr u32 kMaxFramePayload = 16u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// MESSAGES
// ---------------------------------------------------------------------------

struct MsgHello {
    std::string displayName;
    UserId resumeUser = 0; // non-zero = "I was this user before I dropped"
};

struct MsgWelcome {
    UserId user = 0;
    SessionId session = 0;
    // Exactly one connected client may write the project to disk. Without this there
    // are two authorities over the same files and no arbiter: the editor's own Save
    // serializes the live registry directly, bypassing the server entirely.
    bool isWriter = false;
    u16 serverProtocol = kProtocolVersion;
};

struct MsgJoinDoc {
    DocId doc = 0;
};

struct MsgLockRequest {
    EntityKey key;
};

struct MsgLockRelease {
    EntityKey key;
};

// Renews EVERY lock this session holds, not one lock each. A per-lock heartbeat makes
// traffic scale with how much an artist has selected, which is exactly backwards.
struct MsgHeartbeat {
    u32 nowMsHint = 0; // diagnostic only; the SERVER's clock decides expiry
};

// A component-level change. `componentKey` is the serializer's own key ("transform",
// "name", ...) and `json` is that component's serialized form, or empty to REMOVE it.
//
// The server never parses `json`. It stores bytes, assigns order and broadcasts. That
// is what keeps 62 component types from becoming 62 server-side cases.
struct MsgEntityDelta {
    EntityKey key;
    std::string componentKey;
    std::string json;       // empty = remove the component
    Revision baseRevision = 0; // what the client believed when it made the edit
    // NOTE: there is deliberately NO sequence field here. Only the server assigns
    // order, and you cannot retrofit authority onto a format that left room for
    // clients to claim it.
};

struct MsgDeltaApplied {
    EntityKey key;
    std::string componentKey;
    std::string json;
    Revision revision = 0; // the NEW revision, server-assigned
    Seq seq = 0;
    UserId author = 0;
};

struct MsgDeltaRejected {
    EntityKey key;
    std::string componentKey;
    Reject reason = Reject::None;
    Revision currentRevision = 0; // re-base against this and retry
};

// A COMMITTED brush operation. `strokeBlob` is an opaque, engine-encoded paint::Stroke
// - the collaboration layer never looks inside it (see the layering rule).
//
// `layerId` is a STABLE id, never an index. Layer reorder/remove is not recorded as an
// operation, so an index recorded today means something different tomorrow and the
// history can never be migrated.
struct MsgPaintOp {
    CanvasId canvas = 0;
    u32 layerId = 0;
    std::vector<u8> strokeBlob;
};

struct MsgPaintCommitted {
    CanvasId canvas = 0;
    u32 layerId = 0;
    std::vector<u8> strokeBlob;
    Seq seq = 0;
    UserId author = 0;
};

// AN IN-PROGRESS STROKE. Structurally a different message from MsgPaintOp, not the
// same message with an `ephemeral` flag.
//
// If a preview can ever enter the durable log, no later compaction can tell a preview
// apart from truth, and replaying the history produces pixels the artist never
// committed. Making it a separate type means the log APPENDER never sees one.
struct MsgPaintPreview {
    CanvasId canvas = 0;
    u32 layerId = 0;
    std::vector<u8> partialBlob;
};

struct MsgLockGrant {
    LockView view;
    bool expired = false; // true = the lease lapsed rather than being released
};

struct MsgLockDenied {
    EntityKey key;
    Reject reason = Reject::None;
    UserId heldBy = 0;
};

struct MsgPeerJoined {
    UserId user = 0;
    std::string displayName;
};

struct MsgPeerLeft {
    UserId user = 0;
};

// ---------------------------------------------------------------------------
// ENCODE / DECODE
// ---------------------------------------------------------------------------
//
// Every Encode appends a complete framed message to `out`. Every Decode takes the
// payload WITHOUT the header (the caller has already read it to know the kind) and
// returns nullopt if the bytes do not decode - never a partially filled struct, so a
// caller cannot accidentally act on half a message.

void EncodeHello(std::vector<u8>& out, const MsgHello& m);
void EncodeWelcome(std::vector<u8>& out, const MsgWelcome& m);
void EncodeJoinDoc(std::vector<u8>& out, const MsgJoinDoc& m);
void EncodeLockRequest(std::vector<u8>& out, const MsgLockRequest& m);
void EncodeLockRelease(std::vector<u8>& out, const MsgLockRelease& m);
void EncodeHeartbeat(std::vector<u8>& out, const MsgHeartbeat& m);
// --- PROJECT TRANSFER ---------------------------------------------------------
//
// One file at a time, requested by the receiver. The alternative - the host pushing
// everything - was rejected: the receiver is the only side that knows what it already
// has, and a push has no natural backpressure, so a large project would queue hundreds
// of megabytes into a transport whose outbound buffer is unbounded.

struct SyncEntry {
    std::string path;   // project-relative, forward slashes
    u64 size = 0;
    std::string sha256; // lowercase hex - what makes "do I already have this?" answerable
};

// AN ENTITY APPEARING OR DISAPPEARING, which no component delta can express.
//
// MsgEntityDelta mutates one component of an entity the receiver must ALREADY have by
// guid, and empty json removes a COMPONENT, not the entity. So without this, "real time"
// meant real time for objects both sides already had - adding or deleting one simply did
// not travel, and the two scenes silently diverged until someone saved.
//
// DESTROY IS ONE ENTITY, NOT A SUBTREE. Hierarchy lives in `parent`, which is serialized
// as a FILE ROW INDEX and therefore cannot cross machines at all; inventing a recursive
// delete on top of a relationship the wire cannot see would delete the wrong things on
// the far side. The editor deletes children explicitly, so each arrives as its own
// message.
struct MsgEntityLife {
    EntityKey key;
    bool destroy = false;
    std::string name; // on create; ignored on destroy
};

// The accepted version, in server order.
struct MsgEntityLived {
    EntityKey key;
    bool destroy = false;
    std::string name;
    Seq seq = 0;
    UserId author = 0;
};

struct MsgSyncRequest {
    u32 unused = 0; // room to ask for a subset later without a new kind
};

struct MsgSyncManifest {
    std::vector<SyncEntry> files;
};

struct MsgFileRequest {
    std::string path;
};

struct MsgFileChunk {
    std::string path;
    u64 offset = 0;      // where these bytes go; lets the receiver detect a gap
    std::vector<u8> data;
    bool last = false;   // the file is complete after this one
};

// Bytes per FileChunk. Well under kMaxFramePayload on purpose: Emit SILENTLY DISCARDS
// an oversized message (it returns void, so nothing can detect it) and the receiver
// would then wait forever for a chunk that was never sent. 256 KiB also keeps one
// stalled file from monopolising the link, since chunks interleave with scene deltas.
inline constexpr usize kFileChunkBytes = 256u * 1024u;

// A hostile or broken manifest must not be able to make the receiver allocate without
// bound before a single byte is verified.
inline constexpr usize kMaxSyncFiles = 200000;

void EncodeEntityLife(std::vector<u8>& out, const MsgEntityLife& m);
void EncodeEntityLived(std::vector<u8>& out, const MsgEntityLived& m);
std::optional<MsgEntityLife> DecodeEntityLife(const u8* p, usize n);
std::optional<MsgEntityLived> DecodeEntityLived(const u8* p, usize n);

void EncodeSyncRequest(std::vector<u8>& out, const MsgSyncRequest& m);
void EncodeSyncManifest(std::vector<u8>& out, const MsgSyncManifest& m);
void EncodeFileRequest(std::vector<u8>& out, const MsgFileRequest& m);
void EncodeFileChunk(std::vector<u8>& out, const MsgFileChunk& m);
std::optional<MsgSyncRequest> DecodeSyncRequest(const u8* p, usize n);
std::optional<MsgSyncManifest> DecodeSyncManifest(const u8* p, usize n);
std::optional<MsgFileRequest> DecodeFileRequest(const u8* p, usize n);
std::optional<MsgFileChunk> DecodeFileChunk(const u8* p, usize n);

void EncodeEntityDelta(std::vector<u8>& out, const MsgEntityDelta& m);
void EncodeDeltaApplied(std::vector<u8>& out, const MsgDeltaApplied& m);
void EncodeDeltaRejected(std::vector<u8>& out, const MsgDeltaRejected& m);
void EncodePaintOp(std::vector<u8>& out, const MsgPaintOp& m);
void EncodePaintCommitted(std::vector<u8>& out, const MsgPaintCommitted& m);
void EncodePaintPreview(std::vector<u8>& out, const MsgPaintPreview& m);
void EncodeLockGrant(std::vector<u8>& out, const MsgLockGrant& m);
void EncodeLockDenied(std::vector<u8>& out, const MsgLockDenied& m);
void EncodePeerJoined(std::vector<u8>& out, const MsgPeerJoined& m);
void EncodePeerLeft(std::vector<u8>& out, const MsgPeerLeft& m);

std::optional<MsgHello> DecodeHello(const u8* p, usize n);
std::optional<MsgWelcome> DecodeWelcome(const u8* p, usize n);
std::optional<MsgJoinDoc> DecodeJoinDoc(const u8* p, usize n);
std::optional<MsgLockRequest> DecodeLockRequest(const u8* p, usize n);
std::optional<MsgLockRelease> DecodeLockRelease(const u8* p, usize n);
std::optional<MsgHeartbeat> DecodeHeartbeat(const u8* p, usize n);
std::optional<MsgEntityDelta> DecodeEntityDelta(const u8* p, usize n);
std::optional<MsgDeltaApplied> DecodeDeltaApplied(const u8* p, usize n);
std::optional<MsgDeltaRejected> DecodeDeltaRejected(const u8* p, usize n);
std::optional<MsgPaintOp> DecodePaintOp(const u8* p, usize n);
std::optional<MsgPaintCommitted> DecodePaintCommitted(const u8* p, usize n);
std::optional<MsgPaintPreview> DecodePaintPreview(const u8* p, usize n);
std::optional<MsgLockGrant> DecodeLockGrant(const u8* p, usize n);
std::optional<MsgLockDenied> DecodeLockDenied(const u8* p, usize n);
std::optional<MsgPeerJoined> DecodePeerJoined(const u8* p, usize n);
std::optional<MsgPeerLeft> DecodePeerLeft(const u8* p, usize n);

// THREE outcomes, not two. "Not enough bytes yet" and "this header is garbage" are
// completely different situations and MUST NOT share a return value: an oversized
// length mistaken for a partial tail is never consumed, so the reader waits forever
// for bytes that will never make it legal, the receive buffer grows without bound and
// that connection is permanently wedged. (That is exactly what the first version of
// this file did, and the self-test asserted the wedge as if it were correct.)
enum class FrameScan : u8 {
    Incomplete, // a legal header, but the payload has not fully arrived - wait
    Ok,         // a whole frame is present
    Invalid,    // the header cannot be trusted; the stream is unrecoverable
};

// Reads one frame header at `p`. Safe to call on a partially filled stream buffer.
FrameScan PeekFrame(const u8* p, usize available, FrameHeader& out);

// Splits a byte stream into complete frames. `consumed` receives how many bytes were
// used, so a caller can erase exactly that much and keep the partial tail.
//
// `fatal` is set when the stream hit an UNRECOVERABLE header. There is no way to
// resynchronise a length-prefixed stream once a length is untrustworthy - the next
// frame could start anywhere - so the only correct response is to drop that
// connection. A caller that ignores this will spin on the same bad bytes forever.
struct Frame {
    MsgType type = MsgType::Invalid;
    u16 version = 0;
    const u8* payload = nullptr;
    usize size = 0;
};
std::vector<Frame> SplitFrames(const u8* p, usize n, usize& consumed, bool& fatal);

} // namespace hbe::collab
