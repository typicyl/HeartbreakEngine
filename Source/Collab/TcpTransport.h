// Collab/TcpTransport.h - the real one. WinSock2 behind the same two interfaces.
//
// Everything above this file - the server, the client, the lock table, the protocol -
// was written and tested against LoopbackTransport with no network at all. This is the
// payoff: sockets arrive as one more implementation of an interface that already had a
// working one, and --test-tcp runs the SAME collaboration session over real localhost
// TCP to prove the abstraction did not leak.
//
// NON-BLOCKING AND POLLED, never threaded. The engine's registry is single-threaded and
// the collaboration client's callbacks mutate it, so inbound bytes must arrive at one
// defined point in the frame - exactly like the engine's existing deferred command
// queues. A thread here would move that problem rather than solve it, and would make
// every lock and revision race genuinely concurrent instead of merely ordered.
//
// LAYERING: Core/Types.h + WinSock2 + the STL. WinSock is the OS, not the engine, so
// the "no engine includes" rule still holds - a headless server links this and nothing
// graphical.
#pragma once

#include "Collab/SecureChannel.h"
#include "Collab/Transport.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::collab {

// Refcounted WSAStartup/WSACleanup. Both transports hold one; the last one out calls
// WSACleanup. Without the refcount, two transports in one process (which --test-tcp
// creates deliberately) would have the first destructor tear WinSock down under the
// second.
class WinsockScope {
public:
    WinsockScope();
    ~WinsockScope();
    bool Ok() const { return ok_; }

private:
    bool ok_ = false;
};

class TcpServerTransport final : public IServerTransport {
public:
    TcpServerTransport() = default;
    ~TcpServerTransport() override;

    // Encrypts and authenticates every connection accepted after this call. `me` must
    // outlive the transport.
    //
    // NOT THE DEFAULT, and that is a deliberate, narrow exception: --test-tcp and any
    // single-machine session run without it. Anything reachable from off the machine
    // MUST call this - see BindPublic() below, which refuses to open a public port
    // without it.
    void EnableSecurity(const Identity& me, PeerPolicy policy);
    bool Secure() const { return id_ != nullptr; }

    // `port` 0 asks the OS for an ephemeral port - use BoundPort() to find out which.
    // That is what keeps --test-tcp from failing on a machine where something already
    // owns a hardcoded port.
    //
    // The default bind address is LOOPBACK. A hostable-by-default server is how a tool
    // ends up listening to the internet because someone ran it to try a feature out.
    bool Listen(u16 port, const char* bindAddr = "127.0.0.1");

    // Listens on every interface - i.e. from the internet, given a forwarded port.
    // REFUSES unless EnableSecurity() was called first: an unauthenticated public
    // listener hands anyone who finds the port write access to the project, and there is
    // no legitimate reason to want one.
    bool BindPublic(u16 port);
    u16 BoundPort() const { return boundPort_; }
    bool Listening() const { return listen_ != ~usize(0); }
    void Close();

    // Accepts pending connections and moves bytes both ways. Call once per tick BEFORE
    // the server's own Tick, so it sees this tick's arrivals.
    void Poll();

    // IServerTransport
    void PollNewConnections(std::vector<ConnId>& out) override;
    void PollDisconnects(std::vector<ConnId>& out) override;
    void Receive(ConnId c, std::vector<u8>& out) override;
    void Send(ConnId c, const u8* data, usize n) override;
    void Disconnect(ConnId c) override;

    // The key this peer PROVED it holds. False on an unsecured or unknown connection.
    // This, not the display name in MsgHello, is what a host should attribute work to.
    bool PeerKeyOf(ConnId c, PublicKey& out) const;
    bool PeerKey(ConnId c, std::array<u8, 64>& out) const override { return PeerKeyOf(c, out); }

private:
    struct Conn {
        usize sock = ~usize(0);
        std::vector<u8> in;
        // Pending bytes send() could not take. A partial send is NORMAL on a socket
        // whose send buffer is full, and dropping the remainder silently truncates a
        // frame - which the peer then treats as an unrecoverable length. This is the
        // single most common way a hand-rolled TCP layer corrupts a stream.
        std::vector<u8> out;
        bool open = true;
        std::unique_ptr<SecureChannel> tls; // null when the transport is unsecured
        // A secured connection is announced to the server only once it is Open. Before
        // that it is a stranger holding a socket, and handing it to CollabServer would
        // create a session, a UserId and a lock budget for someone who may be refused a
        // millisecond later.
        bool announced = false;
    };
    void Reap(ConnId c);

    WinsockScope wsa_;
    usize listen_ = ~usize(0);
    u16 boundPort_ = 0;
    std::unordered_map<ConnId, Conn> conns_;
    std::vector<ConnId> pendingNew_, pendingGone_;
    ConnId nextId_ = 1;
    const Identity* id_ = nullptr;
    PeerPolicy policy_;
};

class TcpClientTransport final : public IClientTransport {
public:
    TcpClientTransport() = default;
    ~TcpClientTransport() override;

    // Encrypts and authenticates the session. Must be called BEFORE Connect. `expectHost`
    // is this client's own check on who it reached - the SSH known_hosts question. Pass a
    // policy that returns false for an unknown key and the client will refuse to hand a
    // project to an impostor even if the impostor would have accepted it.
    void EnableSecurity(const Identity& me, PeerPolicy expectHost);
    bool Secure() const { return id_ != nullptr; }

    // Blocking connect, deliberately: it happens once, at a moment the user is already
    // waiting ("connecting..."), and a non-blocking connect state machine buys nothing
    // but three extra states to get wrong.
    //
    // `host` accepts a name or an IPv4 literal. Returning true means the SOCKET is up;
    // on a secured client the session is not usable until Connected() reports true,
    // which happens only once both ends have authenticated.
    bool Connect(const char* host, u16 port);

    // True while the socket is up but the peers have not finished proving who they are.
    bool Handshaking() const;
    // Why a secured session failed, for the UI. Empty if there is nothing to report.
    const char* SecurityError() const;
    // The host key this client PROVED it was talking to.
    bool PeerKey(PublicKey& out) const;
    // Moves bytes both ways. Call once per tick before CollabClient::Pump.
    void Poll();

    // IClientTransport
    bool Connected() const override;
    void Receive(std::vector<u8>& out) override;
    void Send(const u8* data, usize n) override;
    void Disconnect() override;

private:
    WinsockScope wsa_;
    usize sock_ = ~usize(0);
    std::vector<u8> in_, out_;
    bool open_ = false;
    const Identity* id_ = nullptr;
    PeerPolicy policy_;
    std::unique_ptr<SecureChannel> tls_;
};

// --test-tcp: runs a real two-client collaboration session over localhost TCP and
// asserts the same invariants --test-collab does over the loopback. Its whole purpose
// is to catch anything the in-process transport was hiding - partial sends, coalesced
// frames, a message split across two recv() calls.
bool TcpTransportSelfTest();

} // namespace hbe::collab
