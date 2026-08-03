// Collab/LockTable.cpp
#include "Collab/LockTable.h"

namespace hbe::collab {

namespace {
// A lease is live when it has been renewed within the timeout. Written as a helper
// because "expired" is asked in four places and one of them getting the comparison
// backwards would hand a live entity to a second editor.
bool LeaseLive(const LockTable::Entry& e, u64 nowMs) {
    if (e.owner == 0) return false;
    // Guard the underflow: a client clock hint or a test that steps time backwards
    // must not make an ancient lease look fresh.
    if (nowMs < e.lastHeartbeatMs) return true;
    return (nowMs - e.lastHeartbeatMs) < kLockTimeoutMs;
}
} // namespace

Reject LockTable::TryAcquire(const EntityKey& k, UserId user, SessionId session, u64 nowMs) {
    if (!k.Valid() || user == 0 || session == 0) return Reject::NotConnected;
    Entry& e = entries_[k];
    if (LeaseLive(e, nowMs)) {
        // Same session: renew and succeed. See the header - a re-select must not error.
        if (e.session == session && e.owner == user) {
            e.lastHeartbeatMs = nowMs;
            return Reject::None;
        }
        // Same USER, new session (they reconnected and immediately clicked the thing
        // they were already holding). Adopt it rather than denying them their own lock.
        if (kReclaimOnReconnect && e.owner == user) {
            e.session = session;
            e.lastHeartbeatMs = nowMs;
            return Reject::None;
        }
        return Reject::HeldByOther;
    }
    // Free, or the previous lease lapsed. Taking over an EXPIRED lease is the normal
    // path, not an error: that is what the timeout is for.
    e.owner = user;
    e.session = session;
    e.lastHeartbeatMs = nowMs;
    return Reject::None;
}

Reject LockTable::Release(const EntityKey& k, UserId user, SessionId session) {
    const auto it = entries_.find(k);
    if (it == entries_.end() || it->second.owner == 0) return Reject::NotOwner;
    // BOTH must match. Matching on user alone would let a stale release from a dropped
    // session drop the lock the user's NEW session legitimately holds - a message that
    // is genuinely in flight during every reconnect.
    if (it->second.owner != user || it->second.session != session) return Reject::NotOwner;
    it->second.owner = 0;
    it->second.session = 0;
    return Reject::None;
}

void LockTable::Heartbeat(SessionId session, u64 nowMs) {
    if (session == 0) return;
    for (auto& [k, e] : entries_)
        if (e.owner != 0 && e.session == session) e.lastHeartbeatMs = nowMs;
}

void LockTable::ExpireStale(u64 nowMs, std::vector<LockView>& expired) {
    for (auto& [k, e] : entries_) {
        if (e.owner == 0) continue;
        if (LeaseLive(e, nowMs)) continue;
        e.owner = 0;
        e.session = 0;
        LockView v;
        v.key = k;
        v.owner = 0;
        v.session = 0;
        v.revision = e.revision;
        expired.push_back(v);
    }
}

void LockTable::SessionEnded(SessionId session) {
    // Deliberately does NOT clear ownership - see the header. The lease keeps running
    // so a reconnect inside the timeout can reclaim it.
    (void)session;
}

void LockTable::ReclaimForUser(UserId user, SessionId newSession, u64 nowMs,
                               const std::function<bool(SessionId)>& isSessionLive,
                               std::vector<LockView>& reclaimed) {
    if (!kReclaimOnReconnect || user == 0 || newSession == 0) return;
    for (auto& [k, e] : entries_) {
        if (e.owner != user) continue;
        if (e.session == newSession) continue; // already ours
        // ONLY reclaim from a session that is actually GONE. Matching on UserId alone
        // transferred locks off a live connection: two windows open under one identity
        // meant the newer one silently stole every lock the older one was holding,
        // while the older one's client still believed it owned them.
        if (isSessionLive && isSessionLive(e.session)) continue;
        if (!LeaseLive(e, nowMs)) continue; // too late; it will expire normally
        e.session = newSession;
        e.lastHeartbeatMs = nowMs;
        LockView v;
        v.key = k;
        v.owner = e.owner;
        v.session = e.session;
        v.revision = e.revision;
        reclaimed.push_back(v);
    }
}

bool LockTable::HoldsLock(const EntityKey& k, UserId user, SessionId session) const {
    const auto it = entries_.find(k);
    if (it == entries_.end()) return false;
    return it->second.owner == user && it->second.session == session && user != 0;
}

Revision LockTable::RevisionOf(const EntityKey& k) const {
    const auto it = entries_.find(k);
    return it == entries_.end() ? 0u : it->second.revision;
}

Revision LockTable::BumpRevision(const EntityKey& k) { return ++entries_[k].revision; }

LockView LockTable::ViewOf(const EntityKey& k) const {
    LockView v;
    v.key = k;
    const auto it = entries_.find(k);
    if (it != entries_.end()) {
        v.owner = it->second.owner;
        v.session = it->second.session;
        v.revision = it->second.revision;
    }
    return v;
}

} // namespace hbe::collab
