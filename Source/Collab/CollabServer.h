// Collab/CollabServer.h - the authority. Validate, order, broadcast.
//
// Every accepted operation passes through exactly one function on one thread and
// leaves with a sequence number. That single total order is what makes "who won the
// race" answerable at all - not timestamps (clocks disagree), not client counters
// (clients lie or lag), not vector clocks (they answer a harder question than this
// problem has).
//
// The server NEVER interprets a component. It stores the key and the JSON bytes,
// assigns order, and broadcasts. That is what keeps 62 component types from becoming
// 62 server-side special cases, and it is why this file includes nothing from Scene/.
#pragma once

#include "Collab/LockTable.h"
#include "Collab/Protocol.h"
#include "Collab/Transport.h"

#include <string>
#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

namespace hbe::collab {

class CollabServer {
public:
    explicit CollabServer(IServerTransport* transport) : transport_(transport) {}

    // ONE tick: accept connections, drain bytes, apply, expire leases, broadcast.
    // `nowMs` is the SERVER's clock and the only clock that decides anything - a
    // client's own time never affects a lease, or a machine with a skewed clock could
    // hold a lock forever or lose one instantly.
    void Tick(u64 nowMs);

    // --- inspection, for tests and for a future admin view ---
    usize PeerCount() const { return peers_.size(); }
    Seq CurrentSeq() const { return seq_; }
    Revision RevisionOf(const EntityKey& k) const { return locks_.RevisionOf(k); }
    LockView LockOf(const EntityKey& k) const { return locks_.ViewOf(k); }
    // The last committed value of a component, as stored. The server keeps this so a
    // client joining late can be brought up to date without a second authority.
    const std::string* ComponentState(const EntityKey& k, const std::string& comp) const;
    // The durable paint history, in server order. In-memory for now; the record
    // framing is already the durable format, so persisting it is an append, not a
    // redesign.
    const std::vector<MsgPaintCommitted>& PaintHistory(CanvasId c) const;
    UserId WriterUser() const { return writer_; }

    // --- THE HOST IS A PEER TOO ------------------------------------------------
    //
    // The person hosting is editing the same scene, in the same process as the
    // authority. They have no transport to speak through, but they must NOT therefore
    // bypass the rules: an edit that skipped the lock table would let the host silently
    // overwrite an entity a guest is holding, which is precisely the situation locks
    // exist to prevent - and the host would never see a refusal, because it never asked.
    //
    // So the host gets a reserved UserId and goes through the SAME LockTable and the
    // same revision counter as everyone else. The only thing it skips is the wire.
    UserId LocalUser() const { return kLocalUser; }
    bool LocalLock(const EntityKey& k, u64 nowMs);
    void LocalUnlock(const EntityKey& k);
    bool LocalHoldsLock(const EntityKey& k) const;
    // Applies the host's own change and broadcasts it. Returns false if the host does
    // not hold the lock - the caller must not have edited, and telling it so is what
    // lets the editor grey the inspector instead of pretending.
    bool LocalDelta(const EntityKey& k, const std::string& componentKey,
                    const std::string& json);

    // Fires for every change accepted FROM A PEER. The host needs it because it is the
    // authority: a guest's edit reaches state_ and every other guest by broadcast, and
    // the host's own registry is the one place nothing tells. Deliberately NOT fired for
    // LocalDelta - the host's own edit is already in its scene, and re-applying it would
    // fight the author's drag.
    std::function<void(const MsgDeltaApplied&)> onApplied;
    // Same reasoning, for an entity APPEARING or DISAPPEARING.
    std::function<void(const MsgEntityLived&)> onLived;
    // A committed stroke from an artist, in server order. The host needs it for the same
    // reason as the others: it is the authority, so nothing broadcasts back to it.
    std::function<void(const MsgPaintCommitted&)> onPainted;

    // The host's own committed stroke. No lock is involved - paint is deliberately not
    // lock-gated, because two artists on one canvas is the point.
    void LocalPaint(CanvasId canvas, u32 layerId, const std::vector<u8>& strokeBlob);

    // The host's own creation/destruction. No lock is required to CREATE (there is no
    // entity to hold yet); destroying REQUIRES the lock, so nobody can delete an object
    // out from under someone who is editing it.
    bool LocalLife(const EntityKey& k, bool destroy, const std::string& name);

    // --- SERVING THE PROJECT ---------------------------------------------------
    //
    // Offers `manifest` to any peer that asks, reading the bytes from `root`.
    //
    // THE MANIFEST IS ALSO THE AUTHORIZATION LIST. A FileRequest naming anything not in
    // it is refused outright, which is what keeps this from becoming "send me any file
    // on your disk" - and it needs no separate path guard, because every entry was
    // produced by our own walk of our own project. (The RECEIVER still contains its
    // paths independently; a peer must never trust the other side's idea of a path.)
    //
    // Not called = nothing is served. THE HEADER USED TO PROMISE A REFUSAL HERE AND THE
    // CODE NEVER DID IT: a host that has not shared answers a SyncRequest with an EMPTY
    // manifest (see OnSyncRequest), which the guest reads as a finished copy. The guest
    // now reports that case by name. Turning it into a distinct refusal would need an
    // appended MsgType, which an older peer skips silently and therefore hangs on - worse
    // than the empty answer.
    void ShareProject(const std::string& root, MsgSyncManifest manifest);
    bool Sharing() const { return !shareRoot_.empty(); }

    // How many peers asked for the project while we had nothing to give. This is the ONLY
    // evidence a host will ever get that someone is waiting: the guest receives a
    // successful-looking empty manifest and never asks again.
    usize AsksWithNothingToSend() const { return asksWithNothingToSend_; }

private:
    // Reserved, and deliberately not 0 (0 means "nobody" throughout) nor drawn from
    // nextUser_ (which starts at 1 for the first guest). A distinct constant means a
    // lock badge can say "you" without a special case anywhere else.
    usize asksWithNothingToSend_ = 0;

    static constexpr UserId kLocalUser = 0xFFFF'FFFFu;
    static constexpr SessionId kLocalSession = 0xFFFF'FFFFu;

    struct Peer {
        ConnId conn = 0;
        UserId user = 0;
        SessionId session = 0;
        std::string name;
        bool helloed = false;
        DocId doc = 0;
        std::vector<u8> inbox; // accumulates partial frames across ticks
        // ONE file in flight per peer, advanced a chunk per tick. Writing a whole file
        // inside the request handler would queue its entire length into a transport
        // whose outbound buffer has no cap - a 500 MB asset would be 500 MB of RAM and
        // would stall every scene delta behind it.
        std::string sendPath;
        u64 sendOffset = 0;
        // The key this connection PROVED, when the transport authenticates. This, not
        // `name`, is who the peer IS.
        std::array<u8, 64> key{};
        bool keyed = false;
    };
    void FinishHello(Peer& p);
    // Hashing a 64-byte key. std::hash has no specialisation for std::array.
    struct KeyHash {
        usize operator()(const std::array<u8, 64>& k) const {
            usize h = 1469598103934665603ull;
            for (const u8 b : k) { h ^= b; h *= 1099511628211ull; }
            return h;
        }
    };
    std::unordered_map<std::array<u8, 64>, UserId, KeyHash> keyUsers_;
    u64 lastTickMs_ = 0;
    void OnSyncRequest(Peer& p);
    void OnFileRequest(Peer& p, const MsgFileRequest& m);
    void PumpFileSends();
    const SyncEntry* SharedEntry(const std::string& path) const;

    std::string shareRoot_;
    MsgSyncManifest shareManifest_;

    void HandleFrame(Peer& p, const Frame& f, u64 nowMs);
    void OnHello(Peer& p, const MsgHello& m, u64 nowMs);
    void OnLockRequest(Peer& p, const MsgLockRequest& m, u64 nowMs);
    void OnLockRelease(Peer& p, const MsgLockRelease& m);
    void OnEntityDelta(Peer& p, const MsgEntityDelta& m);
    void OnEntityLife(Peer& p, const MsgEntityLife& m);
    void OnPaintOp(Peer& p, const MsgPaintOp& m);
    void OnPaintPreview(Peer& p, const MsgPaintPreview& m);

    void SendTo(Peer& p, const std::vector<u8>& bytes);
    void Broadcast(const std::vector<u8>& bytes, ConnId except = 0);

    IServerTransport* transport_ = nullptr;
    std::unordered_map<ConnId, Peer> peers_;
    LockTable locks_;
    Seq seq_ = 0;
    UserId nextUser_ = 1;
    SessionId nextSession_ = 1;
    // Exactly one connected client may write the project to disk (see MsgWelcome).
    // First in wins; cleared when they leave so the next joiner takes it.
    UserId writer_ = 0;
    // key -> componentKey -> json. The authoritative byte store.
    std::unordered_map<EntityKey, std::unordered_map<std::string, std::string>, EntityKeyHash>
        state_;
    std::unordered_map<CanvasId, std::vector<MsgPaintCommitted>> paintLog_;
    // Remembers which UserId a display name had, so a reconnect resumes the same
    // identity and can reclaim its locks. Keyed by name because that is all a client
    // can prove without authentication - which is exactly why authentication belongs
    // in the Hub before this is exposed beyond a trusted LAN.
    std::unordered_map<std::string, UserId> knownUsers_;
};

} // namespace hbe::collab
