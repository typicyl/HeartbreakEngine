// Collab/CollabSelfTest.cpp - `--test-collab`.
//
// Drives the SHIPPING CollabServer and CollabClient over the loopback transport. No
// mocks, no test doubles of the logic under test: the only thing swapped is the
// transport, which is the entire reason the transport is an interface.
//
// Every case asserts that the WRONG behaviour FAILS it - the discipline every other
// self-test in this engine follows. A test that only demonstrates the happy path would
// still pass if locks stopped being enforced.
#include "Collab/CollabClient.h"
#include "Collab/CollabServer.h"
#include "Collab/LoopbackTransport.h"

#include <cstdio>
#include <string>
#include <vector>

namespace hbe::collab {

namespace {

int g_fails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("collab FAIL: %s\n", what);
}

// One "network round trip": both clients push, the server processes, both clients
// drain. Deterministic - which is exactly what a socket cannot give a test.
struct Harness {
    LoopbackHub hub;
    CollabServer server{&hub};
    u64 now = 1000;

    void Step(std::vector<CollabClient*> clients, u64 advanceMs = 16) {
        now += advanceMs;
        for (CollabClient* c : clients) c->Pump(now);
        server.Tick(now);
        for (CollabClient* c : clients) c->Pump(now);
    }
};

EntityKey Key(u64 doc, u64 guid) {
    EntityKey k;
    k.doc = doc;
    k.guid = guid;
    return k;
}

} // namespace

bool CollabSelfTest() {
    g_fails = 0;

    // ------------------------------------------------------------------
    // 1) TWO CLIENTS RACE FOR ONE LOCK. Exactly one wins; the loser is told
    //    who holds it. This is the headline invariant.
    // ------------------------------------------------------------------
    {
        Harness h;
        int grantsA = 0, grantsB = 0, deniedB = 0;
        UserId deniedHolder = 0;

        ClientCallbacks ca;
        ca.onLock = [&](const MsgLockGrant& g) {
            if (g.view.owner != 0) ++grantsA;
        };
        ClientCallbacks cb;
        cb.onLock = [&](const MsgLockGrant& g) {
            if (g.view.owner != 0) ++grantsB;
        };
        cb.onLockDenied = [&](const MsgLockDenied& d) {
            ++deniedB;
            deniedHolder = d.heldBy;
        };

        CollabClient A(h.hub.CreateClient(), ca);
        CollabClient B(h.hub.CreateClient(), cb);
        A.Hello("ana");
        B.Hello("ben");
        h.Step({&A, &B});
        Check(A.Ready() && B.Ready(), "both clients should have been welcomed");
        Check(A.User() != B.User(), "two users must not share a UserId");

        const EntityKey k = Key(7, 42);
        // Both request in the SAME tick - the actual contended case.
        A.RequestLock(k);
        B.RequestLock(k);
        h.Step({&A, &B});

        const UserId owner = h.server.LockOf(k).owner;
        Check(owner != 0, "the contested entity should be locked by SOMEONE");
        Check(owner == A.User() || owner == B.User(), "the lock went to a stranger");
        Check(deniedB == 1 || grantsB == 1, "the second requester got no answer at all");
        if (deniedB == 1) Check(deniedHolder == owner, "the denial named the wrong holder");

        // MUTUAL EXCLUSION, TESTED BEHAVIOURALLY.
        //
        // The obvious assertion - !(A.CanEdit(k) && B.CanEdit(k)) - is a TAUTOLOGY and
        // must not be used: CanEdit compares a single broadcast owner id against the
        // caller's own id, and two clients have different ids, so the conjunction is
        // false by construction. An adversarial review deleted mutual exclusion from
        // the server outright and that assertion still passed.
        //
        // So make BOTH clients actually try to write, and count what the SERVER did.
        // If exclusion were removed, both edits would apply and this fails.
        // Asserted against SERVER state, not client callbacks: the clients copied their
        // callbacks at construction, and server truth is what the test actually cares
        // about anyway.
        A.SendDelta(k, "transform", "{\"from\":\"A\"}");
        B.SendDelta(k, "transform", "{\"from\":\"B\"}");
        h.Step({&A, &B});

        const std::string* val = h.server.ComponentState(k, "transform");
        Check(val != nullptr, "the lock winner's edit did not reach the server");
        if (val) {
            const bool fromWinner = (owner == A.User()) ? (*val == "{\"from\":\"A\"}")
                                                        : (*val == "{\"from\":\"B\"}");
            Check(fromWinner, "the LOSER of the lock race wrote the authoritative state");
        }
        // Exactly one revision bump: two accepted edits would mean both wrote.
        Check(h.server.RevisionOf(k) == 1,
              "the contested entity took more than one accepted edit - both clients wrote");
    }

    // ------------------------------------------------------------------
    // TWO COMPONENTS OF ONE ENTITY, IN ONE ROUND TRIP.
    //
    // The lock owner sends `transform` and `name` back to back, before either
    // acceptance has travelled back. Both must land.
    //
    // This is the case that used to be silently destructive. Revisions are per
    // ENTITY, so the second send carried a baseRevision the server had already
    // moved past, and it was rejected as stale - while MsgDeltaRejected carries
    // no json and the client keeps no copy, so the rename was simply gone. A
    // gizmo drag plus a rename, or any inspector row touching two components,
    // hit it every time.
    // ------------------------------------------------------------------
    {
        Harness h;
        ClientCallbacks cb;
        CollabClient A(h.hub.CreateClient(), cb);
        A.Hello("ana");
        h.Step({&A});

        EntityKey k;
        k.doc = 1;
        k.guid = 4242;
        A.RequestLock(k);
        h.Step({&A});
        Check(h.server.LockOf(k).owner == A.User(), "the sole client should hold the lock");

        A.SendDelta(k, "transform", "{\"p\":[1,2,3]}");
        A.SendDelta(k, "name", "\"Renamed\"");
        h.Step({&A});

        const std::string* t = h.server.ComponentState(k, "transform");
        const std::string* n = h.server.ComponentState(k, "name");
        Check(t != nullptr && *t == "{\"p\":[1,2,3]}",
              "the first component of a two-component change did not land");
        Check(n != nullptr && *n == "\"Renamed\"",
              "THE SECOND COMPONENT OF A TWO-COMPONENT CHANGE WAS LOST - a lock owner "
              "cannot be stale against itself, and a rejection carries no json, so "
              "rejecting it destroys the edit outright");
        Check(h.server.RevisionOf(k) == 2,
              "both edits should have been accepted, bumping the revision twice");
    }

    // ------------------------------------------------------------------
    // 2) A LOCK EXPIRES BY HEARTBEAT TIMEOUT, and the entity becomes
    //    acquirable again. Without this a crashed client freezes an entity
    //    until the server restarts.
    // ------------------------------------------------------------------
    {
        Harness h;
        ClientCallbacks none;
        CollabClient A(h.hub.CreateClient(), none);
        CollabClient B(h.hub.CreateClient(), none);
        A.Hello("ana");
        B.Hello("ben");
        h.Step({&A, &B});

        const EntityKey k = Key(1, 100);
        A.RequestLock(k);
        h.Step({&A, &B});
        Check(h.server.LockOf(k).owner == A.User(), "A should hold the lock");

        // B alone keeps pumping: A has "crashed" and sends no heartbeat.
        for (int i = 0; i < 12; ++i) h.Step({&B}, kLockHeartbeatMs);
        Check(h.server.LockOf(k).owner == 0, "the lease did NOT expire after the timeout");

        B.RequestLock(k);
        h.Step({&B});
        Check(h.server.LockOf(k).owner == B.User(),
              "B could not take the entity after A's lease lapsed");
    }

    // ------------------------------------------------------------------
    // 3) A HELD LOCK DOES NOT EXPIRE while its owner heartbeats. The mirror
    //    of case 2 - without it, "expiry works" could be satisfied by
    //    expiring everything constantly.
    // ------------------------------------------------------------------
    {
        Harness h;
        ClientCallbacks none;
        CollabClient A(h.hub.CreateClient(), none);
        A.Hello("ana");
        h.Step({&A});
        const EntityKey k = Key(1, 7);
        A.RequestLock(k);
        h.Step({&A});
        for (int i = 0; i < 20; ++i) h.Step({&A}, kLockHeartbeatMs);
        Check(h.server.LockOf(k).owner == A.User(),
              "an actively heartbeating client LOST its lock");
    }

    // ------------------------------------------------------------------
    // 4) A NON-OWNER CANNOT EDIT, and an owner can. The lock has to actually
    //    gate writes, not merely display a badge.
    // ------------------------------------------------------------------
    {
        Harness h;
        int appliedA = 0, rejectedB = 0;
        Reject lastReason = Reject::None;
        ClientCallbacks ca;
        ca.onDelta = [&](const MsgDeltaApplied&) { ++appliedA; };
        ClientCallbacks cb;
        cb.onRejected = [&](const MsgDeltaRejected& r) {
            ++rejectedB;
            lastReason = r.reason;
        };
        CollabClient A(h.hub.CreateClient(), ca);
        CollabClient B(h.hub.CreateClient(), cb);
        A.Hello("ana");
        B.Hello("ben");
        h.Step({&A, &B});

        const EntityKey k = Key(3, 9);
        A.RequestLock(k);
        h.Step({&A, &B});

        B.SendDelta(k, "transform", "{\"x\":1}");
        h.Step({&A, &B});
        Check(rejectedB == 1, "a non-owner's edit was not rejected");
        Check(lastReason == Reject::NotOwner, "the rejection reason should be NotOwner");
        Check(h.server.ComponentState(k, "transform") == nullptr,
              "a non-owner's edit REACHED the authoritative state");

        A.SendDelta(k, "transform", "{\"x\":2}");
        h.Step({&A, &B});
        Check(appliedA >= 1, "the lock owner's edit was not applied");
        const std::string* got = h.server.ComponentState(k, "transform");
        Check(got != nullptr && *got == "{\"x\":2}", "the owner's value did not reach the server");
    }

    // ------------------------------------------------------------------
    // 5) THE LOCK OWNER'S RAPID EDITS ALL LAND, AND THE LAST ONE WINS.
    //
    //    THIS ASSERTION WAS REVERSED, DELIBERATELY, AND THE OLD ONE WAS WRONG.
    //    It used to require that the second of two same-revision edits be
    //    REJECTED as stale and that the FIRST value win - and its own comment
    //    said "this is exactly what an editor dragging a gizmo produces the
    //    instant the round trip is slower than the frame rate". That is the
    //    bug, stated as the requirement: while dragging, the value the user
    //    has moved on FROM is kept and the value they actually want is thrown
    //    away, and if they stop moving on a rejected frame the final position
    //    never lands at all.
    //
    //    The lock is what makes it safe to accept them. It is exclusive, so no
    //    other peer can have written in between; the only revisions in the gap
    //    are this client's OWN earlier edits still in flight. Rejecting those
    //    is not conflict detection, it is a client arguing with itself - and
    //    for two DIFFERENT components it silently destroyed the second edit,
    //    because MsgDeltaRejected carries no json and the client keeps no copy.
    //
    //    Real conflict detection is the lock, and case 4 above tests it: a
    //    non-owner's write is refused.
    // ------------------------------------------------------------------
    {
        Harness h;
        int rejected = 0;
        ClientCallbacks ca;
        ca.onRejected = [&](const MsgDeltaRejected&) { ++rejected; };
        CollabClient A(h.hub.CreateClient(), ca);
        A.Hello("ana");
        h.Step({&A});

        const EntityKey k = Key(4, 11);
        A.RequestLock(k);
        h.Step({&A});
        A.SendDelta(k, "name", "\"first\"");
        h.Step({&A});
        const Revision afterFirst = h.server.RevisionOf(k);
        Check(afterFirst > 0, "an accepted edit did not bump the revision");

        // Three edits before a single pump - a gizmo drag at 120fps over a link whose
        // round trip is longer than a frame.
        A.SendDelta(k, "name", "\"second\"");
        A.SendDelta(k, "name", "\"third\"");
        A.SendDelta(k, "name", "\"fourth\"");
        h.Step({&A});

        Check(rejected == 0,
              "the lock owner's own in-flight edits must not be rejected - it cannot be "
              "stale against itself");
        const std::string* now = h.server.ComponentState(k, "name");
        Check(now != nullptr && *now == "\"fourth\"",
              "THE LAST VALUE MUST WIN. Keeping an earlier one means a drag ends on "
              "whatever position happened to be acknowledged, not where the user let go");
        Check(h.server.RevisionOf(k) == afterFirst + 3,
              "every accepted edit should bump the revision");
    }

    // ------------------------------------------------------------------
    // 6) PAINT OPS COMMIT TO THE HISTORY IN SERVER ORDER, from both artists,
    //    with attribution - and PREVIEWS NEVER ENTER IT.
    // ------------------------------------------------------------------
    {
        Harness h;
        int paintsSeenByB = 0, previewsSeenByB = 0;
        ClientCallbacks ca;
        ClientCallbacks cb;
        cb.onPaint = [&](const MsgPaintCommitted&) { ++paintsSeenByB; };
        cb.onPaintPreview = [&](const MsgPaintPreview&) { ++previewsSeenByB; };
        CollabClient A(h.hub.CreateClient(), ca);
        CollabClient B(h.hub.CreateClient(), cb);
        A.Hello("ana");
        B.Hello("ben");
        h.Step({&A, &B});

        const CanvasId canvas = 77;
        A.SendPaintOp(canvas, 1, {1, 2, 3});
        h.Step({&A, &B});
        B.SendPaintOp(canvas, 1, {4, 5});
        h.Step({&A, &B});
        A.SendPaintPreview(canvas, 1, {9, 9, 9});
        h.Step({&A, &B});

        const std::vector<MsgPaintCommitted>& log = h.server.PaintHistory(canvas);
        Check(log.size() == 2, "the paint history should hold exactly the 2 COMMITTED ops");
        if (log.size() == 2) {
            Check(log[0].seq < log[1].seq, "the history is not in server order");
            Check(log[0].author == A.User() && log[1].author == B.User(),
                  "paint ops lost their attribution");
            Check(log[0].strokeBlob == std::vector<u8>({1, 2, 3}),
                  "the stroke blob was not preserved byte-for-byte");
        }
        // THE INVARIANT THAT MATTERS: a preview must never be durable.
        for (const MsgPaintCommitted& c : log)
            Check(c.strokeBlob != std::vector<u8>({9, 9, 9}),
                  "a PREVIEW stroke reached the durable history");
        Check(paintsSeenByB >= 1, "B never saw A's committed stroke");
        Check(previewsSeenByB == 1, "B did not receive A's live preview");
    }

    // ------------------------------------------------------------------
    // 7) A RECONNECTING USER KEEPS THEIR IDENTITY AND RECLAIMS THEIR LOCKS.
    //    Without this a two-second blink costs an artist everything they held.
    // ------------------------------------------------------------------
    {
        Harness h;
        ClientCallbacks none;
        LoopbackClientTransport* ta = h.hub.CreateClient();
        CollabClient A(ta, none);
        A.Hello("ana");
        h.Step({&A});
        const UserId originalUser = A.User();
        const EntityKey k = Key(5, 55);
        A.RequestLock(k);
        h.Step({&A});
        Check(h.server.LockOf(k).owner == originalUser, "A should hold the lock");

        ta->Disconnect();
        h.Step({}, 100); // the server observes the drop
        Check(h.server.LockOf(k).owner == originalUser,
              "the lock was dropped immediately on disconnect - a blink must not cost it");

        CollabClient A2(h.hub.CreateClient(), none);
        A2.Hello("ana", originalUser);
        h.Step({&A2});
        Check(A2.User() == originalUser, "a reconnecting user did not keep their UserId");
        Check(h.server.LockOf(k).owner == originalUser, "the reclaimed lock changed owner");
        Check(h.server.LockOf(k).session == A2.Session(),
              "the lock was not re-pointed at the NEW session");
        Check(A2.CanEdit(k), "the reconnected client cannot edit what it still holds");
    }

    // ------------------------------------------------------------------
    // 8) FRAMING: a byte stream split at every possible boundary still yields
    //    exactly the same messages. The loopback coalesces like TCP, so this
    //    is the property that stops a socket from breaking everything later.
    // ------------------------------------------------------------------
    {
        std::vector<u8> stream;
        EncodeHello(stream, MsgHello{"ana", 0});
        EncodeLockRequest(stream, MsgLockRequest{Key(1, 2)});
        EncodePaintOp(stream, MsgPaintOp{9, 3, {7, 7, 7, 7}});

        usize consumed = 0;
        bool fatal = false;
        const std::vector<Frame> whole =
            SplitFrames(stream.data(), stream.size(), consumed, fatal);
        Check(whole.size() == 3, "three encoded messages did not split into three frames");
        Check(consumed == stream.size(), "splitting whole frames did not consume the buffer");

        // Feed it one byte at a time; a frame must only appear once ALL of it has.
        for (usize cut = 1; cut < stream.size(); ++cut) {
            usize used = 0;
            bool pf = false;
            const std::vector<Frame> partial = SplitFrames(stream.data(), cut, used, pf);
            Check(used <= cut, "SplitFrames consumed more than it was given");
            Check(partial.size() <= whole.size(), "a partial buffer produced EXTRA frames");
            // Whatever it did return must be byte-identical to the same prefix.
            for (usize i = 0; i < partial.size(); ++i)
                Check(partial[i].type == whole[i].type && partial[i].size == whole[i].size,
                      "a frame decoded differently when the stream was split");
        }

        // A truncated header must yield nothing rather than reading past the end.
        usize u2 = 0;
        bool f2 = false;
        Check(SplitFrames(stream.data(), 3, u2, f2).empty(),
              "a partial frame header produced a frame");
        Check(u2 == 0, "a partial header consumed bytes");
    }

    // ------------------------------------------------------------------
    // 9) AN UNKNOWN MESSAGE KIND IS SKIPPED, NOT FATAL. This is what makes
    //    every other protocol decision reversible: a newer peer can send
    //    something this build predates and the stream stays in sync.
    // ------------------------------------------------------------------
    {
        std::vector<u8> stream;
        // Hand-frame a message with a kind this build will never know.
        const u32 len = 4;
        const u16 kind = 4242, ver = kProtocolVersion;
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&len),
                      reinterpret_cast<const u8*>(&len) + 4);
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&kind),
                      reinterpret_cast<const u8*>(&kind) + 2);
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&ver),
                      reinterpret_cast<const u8*>(&ver) + 2);
        stream.insert(stream.end(), {0xDE, 0xAD, 0xBE, 0xEF});
        // ...followed by one this build DOES know.
        EncodeLockRelease(stream, MsgLockRelease{Key(6, 6)});

        usize consumed = 0;
        bool fatal = false;
        const std::vector<Frame> frames =
            SplitFrames(stream.data(), stream.size(), consumed, fatal);
        Check(!fatal, "an unknown KIND must not be fatal - only a bad LENGTH is");
        Check(frames.size() == 2, "an unknown kind broke the stream instead of being skipped");
        Check(consumed == stream.size(), "the unknown frame's bytes were not consumed");
        if (frames.size() == 2) {
            Check(frames[0].type == MsgType::Invalid, "the unknown kind should decode as Invalid");
            Check(frames[1].type == MsgType::LockRelease,
                  "the message AFTER an unknown kind was not recovered");
        }
    }

    // ------------------------------------------------------------------
    // 10) A HOSTILE LENGTH IS REFUSED BEFORE ALLOCATING. Without the cap, a
    //     corrupt frame header is an instant multi-gigabyte reserve.
    // ------------------------------------------------------------------
    {
        std::vector<u8> stream;
        const u32 len = 0xFFFFFFFFu;
        const u16 kind = static_cast<u16>(MsgType::Hello), ver = kProtocolVersion;
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&len),
                      reinterpret_cast<const u8*>(&len) + 4);
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&kind),
                      reinterpret_cast<const u8*>(&kind) + 2);
        stream.insert(stream.end(), reinterpret_cast<const u8*>(&ver),
                      reinterpret_cast<const u8*>(&ver) + 2);
        usize consumed = 0;
        bool fatal = false;
        Check(SplitFrames(stream.data(), stream.size(), consumed, fatal).empty(),
              "a frame claiming 4 GiB was accepted");
        // THE POINT OF THIS CASE, and the thing the first version got wrong: an
        // oversized length must be reported as UNRECOVERABLE. The original code
        // returned "not enough bytes yet", so the reader waited forever for bytes that
        // could never make the frame legal - the connection wedged and its buffer grew
        // without bound. The old test asserted consumed==0 and called that correct.
        Check(fatal, "a hostile length was treated as a partial tail instead of fatal");
        FrameHeader h;
        Check(PeekFrame(stream.data(), stream.size(), h) == FrameScan::Invalid,
              "PeekFrame did not report an over-cap length as Invalid");
        // ...and a genuinely partial header is INCOMPLETE, not Invalid - the two must
        // stay distinguishable or every slow connection looks hostile.
        FrameHeader h2;
        Check(PeekFrame(stream.data(), 3, h2) == FrameScan::Incomplete,
              "a partial header should be Incomplete, not Invalid");
    }

    // ------------------------------------------------------------------
    // 11) A TRUNCATED PAYLOAD DECODES TO NOTHING, never to a half-filled
    //     struct a caller might act on.
    // ------------------------------------------------------------------
    {
        std::vector<u8> one;
        EncodeEntityDelta(one, MsgEntityDelta{Key(1, 1), "transform", "{\"a\":1}", 3});
        const u8* payload = one.data() + kFrameHeaderBytes;
        const usize n = one.size() - kFrameHeaderBytes;
        Check(DecodeEntityDelta(payload, n).has_value(), "a whole delta failed to decode");
        for (usize cut = 0; cut < n; ++cut)
            Check(!DecodeEntityDelta(payload, cut).has_value(),
                  "a TRUNCATED delta decoded successfully");
    }

    if (g_fails == 0) {
        std::printf("collab: lock race resolves to one owner; leases expire and renew; "
                    "non-owners cannot write; a lock owner's rapid and multi-component edits all land with the LAST value winning; paint ops "
                    "commit in order and previews never do; reconnect reclaims; framing "
                    "survives arbitrary splits, unknown kinds and hostile lengths\n");
    }
    return g_fails == 0;
}

} // namespace hbe::collab
