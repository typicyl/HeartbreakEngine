// Collab/CollabTypes.h - the collaboration layer's whole vocabulary.
//
// LAYERING RULE, enforced by inspection because CMake cannot check it: every file
// under Source/Collab/ includes Core/Types.h, Core/BinaryStream.h and the STL.
// NOTHING ELSE from the engine. No entt, no glm, no Scene/, no Renderer/, no imgui.
//
//   grep -rE '#include "(Scene|Renderer|RHI|Editor|Engine|UI|Assets|Game)/' Source/Collab/
//
// must stay empty. This is not tidiness. Admit Scene/PaintSystem.h here (it pulls
// glm/glm.hpp) and three things break at once: a headless server needs a GPU-capable
// build to link, the wire protocol re-versions every time `.hbpaint` bumps, and a
// component struct becomes reachable from the server - at which point the server has
// an opinion about what a component MEANS and stops being a byte store. Everything
// engine-shaped crosses this boundary as a u64, a std::string of JSON, or an opaque
// std::vector<u8>.
#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace hbe::collab {

inline constexpr u16 kProtocolVersion = 1;

// ---------------------------------------------------------------------------
// IDENTITY
// ---------------------------------------------------------------------------

// A HUMAN. Assigned by the server and recorded in the durable paint history, so
// attribution survives a reconnect and a server restart.
//
// Deliberately NOT a SessionId: a session dies every time someone's network blinks,
// and a history whose author field dies with the connection cannot answer "whose
// stroke was this?" tomorrow.
using UserId = u32; // 0 = nobody / the server itself

// ONE CONNECTION INSTANCE. A user who reconnects keeps their UserId and gets a NEW
// SessionId. That difference is exactly what lock reclaim keys on: "the same person
// is back" (reclaim their locks) versus "the same connection" (never true after a
// drop). Never written to a durable record.
using SessionId = u32; // 0 = invalid

// ONE AUTHORED DOCUMENT (a .hbscene). MINTED ONCE and stored INSIDE the file.
//
// Deliberately NOT derived from the path. This engine already made the path-keying
// mistake once and wrote the post-mortem into Assets/SlotIds.h: the cooker derived
// pack slots from an Assets-relative path key and shipped pack stability was exactly
// zero, because path-keying makes a rename indistinguishable from delete + create.
// Here the failure is worse than unstable packs - rename a scene and every DocId in
// every persisted history becomes unaddressable.
//
// Do NOT reuse packSlot for this: a deleted asset's slot is freed and reused, so a
// deleted scene would hand its entire history to the next scene created.
using DocId = u64; // 0 = invalid

// ONE PAINT CANVAS. Minted once, stored in the `.hbpaint`. Same reasoning as DocId:
// a canvas's only identity today is a path string, and a durable stroke history keyed
// on a path that an undo can change is not durable.
using CanvasId = u64; // 0 = invalid

// The server's TOTAL ORDER. Every accepted operation gets the next value. This is the
// only clock in the system - no wall time, no client counters, no vector clocks.
// Monotonic within a server lifetime and persisted with the history.
using Seq = u64; // 0 = "before anything happened"

// A per-entity version. Bumped by the server every time that entity changes. A client
// edit carries the revision it was based on; a mismatch is a stale edit.
using Revision = u32;

// ---------------------------------------------------------------------------
// ADDRESSING
// ---------------------------------------------------------------------------

// THE key for a lock, a revision and a delta. Not an entt handle: those are indices
// into one process's registry, they recycle, and they do not survive a scene reload -
// all three of which are fatal for something a second machine has to name.
struct EntityKey {
    DocId doc = 0;
    u64 guid = 0; // Scene/EntityGuid's stable per-entity id

    bool Valid() const { return doc != 0 && guid != 0; }
    bool operator==(const EntityKey& o) const { return doc == o.doc && guid == o.guid; }
    bool operator!=(const EntityKey& o) const { return !(*this == o); }
};

// Hash for unordered_map. Splitmix-style finalizer on the two halves: guids are
// minted sequentially in some projects, and a plain XOR would collide every entity in
// document A with the same-index entity in document B.
struct EntityKeyHash {
    usize operator()(const EntityKey& k) const noexcept {
        auto mix = [](u64 x) {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return x;
        };
        return static_cast<usize>(mix(k.doc) ^ (mix(k.guid) + 0x9e3779b97f4a7c15ULL));
    }
};

// ---------------------------------------------------------------------------
// LOCKS
// ---------------------------------------------------------------------------

// What a client is told about one entity's lock. Non-owners get this too - the whole
// point is that everyone SEES the lock (and who holds it) while only one may write.
struct LockView {
    EntityKey key;
    UserId owner = 0;       // 0 = unlocked
    SessionId session = 0;  // which connection holds it (for reclaim/ABA)
    Revision revision = 0;  // the entity's current revision, server-authoritative
};

// Why the server said no. A client must be able to react differently to "someone else
// has it" (show whose) and "your edit was stale" (re-base and retry), so these are
// distinct codes rather than one bool.
enum class Reject : u8 {
    None = 0,
    NotConnected,     // no session / not joined to that document
    HeldByOther,      // another user owns the lock
    NotOwner,         // you tried to edit or release something you do not hold
    StaleRevision,    // your edit was based on a revision that is no longer current
    UnknownEntity,    // the server has never heard of that key
    ReadOnlySession,  // you are connected but not the writer (see kWriterElection)
    Malformed,        // the frame did not decode
    Count
};
const char* RejectName(Reject r);

// ---------------------------------------------------------------------------
// TUNABLES - all in one place so a deployment can reason about them together.
// ---------------------------------------------------------------------------

// A lock is a LEASE, not a handshake. A client that crashes, sleeps its laptop or
// loses its network cannot release anything, so an un-renewed lock must fall away on
// its own or that entity is dead until the server restarts.
//
// The heartbeat is deliberately several times shorter than the timeout: one dropped
// or late heartbeat must not cost a lock that is being actively edited.
inline constexpr u32 kLockHeartbeatMs = 2000;
inline constexpr u32 kLockTimeoutMs = 10000;

// A reconnecting user reclaims locks their PREVIOUS session held, as long as the lease
// has not expired. Without this, a two-second network blink costs an artist every lock
// they were holding and hands their in-progress entity to whoever asks next.
inline constexpr bool kReclaimOnReconnect = true;

} // namespace hbe::collab
