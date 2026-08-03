// Collab/CollabClient.h - CLIENT. What the editor talks to.
//
// The editor is a CLIENT of this, not the other way round. Nothing in this file knows
// what a Scene, an entt registry, a Renderer or an ImGui panel is; inbound changes are
// queued and handed to the host through a plain callback interface, and the host
// applies them at ONE point in its frame.
//
// That queue-and-drain shape is not a preference - it is the engine's existing rule.
// The registry is single-threaded, the job system does not guarantee otherwise, and
// the engine already has deferred command queues drained at a defined point each frame
// for exactly this reason. A transport that applied changes the instant bytes arrived
// would be mutating the registry from whatever thread the socket ran on.
#pragma once

#include "Collab/Protocol.h"
#include "Collab/Transport.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::collab {

// What the host implements to receive changes. Deliberately callbacks rather than a
// pure-virtual host interface with engine types: this file must stay free of Scene/.
struct ClientCallbacks {
    // An accepted change to a component, in server order. Apply it to the entity with
    // this guid. `json` empty = remove the component.
    std::function<void(const MsgDeltaApplied&)> onDelta;
    // Your own change lost. Re-base on `currentRevision` and retry, or drop it.
    std::function<void(const MsgDeltaRejected&)> onRejected;
    // A lock changed anywhere - yours or someone else's. Drives the "held by" badge.
    std::function<void(const MsgLockGrant&)> onLock;
    std::function<void(const MsgLockDenied&)> onLockDenied;
    // A committed stroke, in server order. Append it to the canvas and rebake.
    std::function<void(const MsgPaintCommitted&)> onPaint;
    // An in-progress stroke from another artist. Draw it; NEVER record it.
    std::function<void(const MsgPaintPreview&)> onPaintPreview;
    std::function<void(const MsgPeerJoined&)> onPeerJoined;
    std::function<void(const MsgPeerLeft&)> onPeerLeft;
    // The host's file list, in reply to RequestProject().
    // An entity appeared or disappeared, in server order.
    std::function<void(const MsgEntityLived&)> onLived;
    std::function<void(const MsgSyncManifest&)> onManifest;
    // Bytes of a file we asked for. In order, one file at a time.
    std::function<void(const MsgFileChunk&)> onFileChunk;
};

class CollabClient {
public:
    CollabClient(IClientTransport* transport, ClientCallbacks cb)
        : transport_(transport), cb_(std::move(cb)) {}

    // Announce ourselves. `resumeUser` is 0 for a first connection; a reconnecting
    // client passes the UserId it was given, so the server can hand back the locks it
    // still holds instead of stranding them for the whole lease.
    void Hello(const std::string& displayName, UserId resumeUser = 0);
    void JoinDocument(DocId doc);

    // ONE drain point per frame. The host calls this where its own deferred queues are
    // drained; every callback above fires from inside here, on the host's thread.
    void Pump(u64 nowMs);

    // --- editing -------------------------------------------------------------
    void RequestLock(const EntityKey& k);
    void ReleaseLock(const EntityKey& k);
    // Send a component change. `baseRevision` must be what this client last saw for
    // that entity - KnownRevision() supplies it, and using anything else is how a
    // client desyncs on its second edit.
    void SendDelta(const EntityKey& k, const std::string& componentKey,
                   const std::string& json);

    // --- painting ------------------------------------------------------------
    void SendPaintOp(CanvasId canvas, u32 layerId, const std::vector<u8>& strokeBlob);
    void SendPaintPreview(CanvasId canvas, u32 layerId, const std::vector<u8>& partial);

    // --- getting the project from someone who has it -------------------------
    // Asks for the file list. The reply arrives on onManifest.
    // Tell everyone an entity was created or destroyed here.
    void SendLife(const EntityKey& k, bool destroy, const std::string& name);

    void RequestProject();
    // Asks for one file. Chunks arrive on onFileChunk until `last`. Request them ONE AT
    // A TIME: the server serves one per peer and would otherwise just forget the first.
    void RequestFile(const std::string& path);

    // --- local view of server truth -----------------------------------------
    UserId User() const { return user_; }
    SessionId Session() const { return session_; }
    bool IsWriter() const { return isWriter_; }
    bool Ready() const { return user_ != 0; }
    Revision KnownRevision(const EntityKey& k) const;
    // 0 = nobody. Non-owners use this to grey the inspector and name the holder.
    UserId LockOwner(const EntityKey& k) const;
    bool CanEdit(const EntityKey& k) const;

private:
    void Dispatch(const Frame& f);
    void Flush();

    IClientTransport* transport_ = nullptr;
    ClientCallbacks cb_;
    std::vector<u8> inbox_, outbox_;
    UserId user_ = 0;
    SessionId session_ = 0;
    bool isWriter_ = false;
    u64 lastHeartbeatMs_ = 0;
    struct Known {
        Revision revision = 0;
        UserId owner = 0;
    };
    std::unordered_map<EntityKey, Known, EntityKeyHash> known_;
};

} // namespace hbe::collab
