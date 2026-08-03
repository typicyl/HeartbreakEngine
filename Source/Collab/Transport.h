// Collab/Transport.h - the seam that hides sockets.
//
// The server and client below are written against these two interfaces and never
// against a socket, so the ENTIRE protocol - locks, revisions, ordering, the paint
// history - is exercised by --test-collab with no network stack, no ports, no
// asynchronous teardown and no flakiness. Sockets become one more implementation of
// an interface that already has a working one, rather than a rewrite.
//
// BYTE STREAMS, NOT MESSAGES. Send takes bytes and Poll returns bytes, because that is
// what TCP will actually give us: a stream that coalesces and splits writes wherever
// it likes. A message-shaped transport interface would let the loopback silently
// preserve message boundaries that a socket does not, and every framing bug would be
// invisible until the day the socket landed. Protocol::SplitFrames is therefore
// exercised on the loopback path too.
#pragma once

#include "Collab/CollabTypes.h"

#include <array>
#include <vector>

namespace hbe::collab {

// One connected peer, from the server's point of view.
using ConnId = u32; // 0 = invalid

// What the server sees. Implementations are not required to be thread-safe: the
// server is single-threaded by design, and inbound bytes are drained at one defined
// point per tick, exactly like the engine's existing deferred command queues.
class IServerTransport {
public:
    virtual ~IServerTransport() = default;

    // Connections that appeared since the last call. Returned once each.
    virtual void PollNewConnections(std::vector<ConnId>& out) = 0;
    // Connections that dropped since the last call. Returned once each. A transport
    // MUST report a drop even when it never delivered a byte - lock reclaim and lease
    // expiry both depend on the server learning that a peer is gone.
    virtual void PollDisconnects(std::vector<ConnId>& out) = 0;
    // Appends every byte received from `c` since the last call. Bytes may be a partial
    // frame; the caller accumulates and re-splits.
    virtual void Receive(ConnId c, std::vector<u8>& out) = 0;
    virtual void Send(ConnId c, const u8* data, usize n) = 0;
    virtual void Disconnect(ConnId c) = 0;

    // The public key this connection PROVED it holds, if the transport authenticates at
    // all. False for the loopback and for an unsecured socket.
    //
    // WHY THE TRANSPORT AND NOT THE PROTOCOL. Only the transport witnessed the
    // handshake. A key sent as a message is a claim - anyone can type someone else's
    // key into a Hello - whereas the transport watched a signature over a fresh nonce.
    // The server keys a UserId on THIS, not on the display name, so a name is a label
    // and nothing more.
    //
    // Defaulted rather than pure so LoopbackTransport and the plain TCP transport stay
    // exactly as they are; a transport that cannot authenticate says so by not
    // overriding it.
    virtual bool PeerKey(ConnId c, std::array<u8, 64>& out) const {
        (void)c;
        (void)out;
        return false;
    }
};

// What the client sees.
class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    virtual bool Connected() const = 0;
    virtual void Receive(std::vector<u8>& out) = 0;
    virtual void Send(const u8* data, usize n) = 0;
    virtual void Disconnect() = 0;
};

} // namespace hbe::collab
