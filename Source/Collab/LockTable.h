// Collab/LockTable.h - SERVER. Who owns what, and for how much longer.
//
// A lock here is a LEASE, not a handshake. The client that holds it must keep saying
// so; if it stops - crash, sleeping laptop, dead network - the lease lapses and the
// entity becomes editable again. The alternative is an entity that stays locked until
// someone restarts the server, which in a small studio means "until tomorrow".
//
// SERVER-SIDE ONLY. A client mirrors what it is told; it never decides ownership,
// because two clients deciding independently is exactly the race this exists to stop.
#pragma once

#include "Collab/CollabTypes.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace hbe::collab {

class LockTable {
public:
    struct Entry {
        UserId owner = 0;
        SessionId session = 0;
        u64 lastHeartbeatMs = 0;
        Revision revision = 0; // the entity's current revision - server truth
    };

    // Attempts to take the lock for (user, session).
    //
    // RE-ENTRANT ON PURPOSE: the same session asking again succeeds and renews. A
    // client that re-selects an entity it already holds must not be told "denied", or
    // the editor has to track lock state perfectly to avoid a spurious error - and if
    // it could do that reliably it would not need a server.
    Reject TryAcquire(const EntityKey& k, UserId user, SessionId session, u64 nowMs);

    // Only the holder may release. A stale release from a previous session is ignored
    // rather than honoured: after a reconnect, the OLD session's release message can
    // still be in flight while the NEW session legitimately holds the lock.
    Reject Release(const EntityKey& k, UserId user, SessionId session);

    // Renews every lock held by `session`. One call per client per heartbeat, not one
    // per lock: per-lock renewal makes traffic scale with selection size.
    void Heartbeat(SessionId session, u64 nowMs);

    // Drops every lease that has not been renewed within kLockTimeoutMs and returns
    // what it dropped so the server can broadcast. Call once per tick.
    void ExpireStale(u64 nowMs, std::vector<LockView>& expired);

    // A session ended. Locks are NOT dropped immediately when kReclaimOnReconnect is
    // set: they keep ticking, so a user who reconnects inside the timeout gets them
    // back. Returns nothing to broadcast - the lease will expire on its own if they
    // do not come back, and broadcasting "released" now would be a lie the moment
    // they reconnect.
    void SessionEnded(SessionId session);

    // A reconnecting user adopts the locks their previous session held. Returns the
    // views to broadcast so every peer's "held by" badge follows the new session.
    // `isSessionLive` answers "is that OTHER session still connected?". Without it
    // this matched on UserId alone and would transfer a lock off a session that is
    // STILL ONLINE - so a second connection under the same identity silently stole
    // every lock the first was actively holding, and the victim's editor kept showing
    // itself as the owner while its edits were refused.
    void ReclaimForUser(UserId user, SessionId newSession, u64 nowMs,
                        const std::function<bool(SessionId)>& isSessionLive,
                        std::vector<LockView>& reclaimed);

    // Ownership test used by the delta path. Deliberately separate from TryAcquire so
    // an edit never silently ACQUIRES a lock as a side effect of being applied.
    bool HoldsLock(const EntityKey& k, UserId user, SessionId session) const;

    Revision RevisionOf(const EntityKey& k) const;
    // Bumps and returns the new revision. Called only after an edit is accepted.
    Revision BumpRevision(const EntityKey& k);

    LockView ViewOf(const EntityKey& k) const;
    usize Count() const { return entries_.size(); }

private:
    // An entry exists for any entity that has EVER been locked or edited, because it
    // also carries the revision. `owner == 0` means "known, not currently locked".
    std::unordered_map<EntityKey, Entry, EntityKeyHash> entries_;
};

} // namespace hbe::collab
