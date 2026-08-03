// Collab/TcpTransport.cpp
//
// winsock2.h MUST precede windows.h - the old winsock.h is pulled in by windows.h and
// the two define the same symbols incompatibly. This is the one ordering rule that
// produces a hundred confusing redefinition errors when broken.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Collab/TcpTransport.h"

#include "Collab/CollabClient.h"
#include "Collab/CollabServer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace hbe::collab {

namespace {

constexpr usize kInvalid = ~usize(0);
int g_wsaRefs = 0;

SOCKET S(usize s) { return static_cast<SOCKET>(s); }

void SetNonBlocking(SOCKET s) {
    u_long mode = 1;
    ::ioctlsocket(s, FIONBIO, &mode);
}

// True when the last WinSock error means "nothing to do right now" rather than a real
// failure. Treating WSAEWOULDBLOCK as an error is how a non-blocking socket layer ends
// up closing every healthy connection.
bool WouldBlock() { return ::WSAGetLastError() == WSAEWOULDBLOCK; }

} // namespace

WinsockScope::WinsockScope() {
    if (g_wsaRefs++ == 0) {
        WSADATA d{};
        ok_ = ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
        if (!ok_) --g_wsaRefs;
    } else {
        ok_ = true;
    }
}

WinsockScope::~WinsockScope() {
    if (ok_ && --g_wsaRefs == 0) ::WSACleanup();
}

// --- server ------------------------------------------------------------------

TcpServerTransport::~TcpServerTransport() { Close(); }

void TcpServerTransport::EnableSecurity(const Identity& me, PeerPolicy policy) {
    id_ = &me;
    policy_ = std::move(policy);
}

bool TcpServerTransport::BindPublic(u16 port) {
    if (!Secure()) return false; // see the header - this is not a recoverable mistake
    return Listen(port, "0.0.0.0");
}

bool TcpServerTransport::PeerKeyOf(ConnId c, PublicKey& out) const {
    const auto it = conns_.find(c);
    if (it == conns_.end() || !it->second.tls) return false;
    if (it->second.tls->State() != ChannelState::Open) return false;
    out = it->second.tls->PeerKey();
    return true;
}

bool TcpServerTransport::Listen(u16 port, const char* bindAddr) {
    if (!wsa_.Ok()) return false;
    const SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;

    // SO_REUSEADDR so a restarted server is not locked out by its own sockets sitting
    // in TIME_WAIT - otherwise "restart the server" fails for up to four minutes.
    BOOL yes = TRUE;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                 sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, bindAddr, &addr.sin_addr) != 1) {
        ::closesocket(ls);
        return false;
    }
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        ::closesocket(ls);
        return false;
    }
    // Read back what we actually got - with port 0 the OS chose one and the caller has
    // no other way to learn it.
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(ls, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
        boundPort_ = ::ntohs(bound.sin_port);

    SetNonBlocking(ls);
    listen_ = static_cast<usize>(ls);
    return true;
}

void TcpServerTransport::Close() {
    for (auto& [id, c] : conns_)
        if (c.sock != kInvalid) ::closesocket(S(c.sock));
    conns_.clear();
    if (listen_ != kInvalid) {
        ::closesocket(S(listen_));
        listen_ = kInvalid;
    }
}

void TcpServerTransport::Poll() {
    if (listen_ == kInvalid) return;

    // Accept everything pending, not just one: a burst of joins would otherwise take
    // one tick each.
    for (;;) {
        const SOCKET s = ::accept(S(listen_), nullptr, nullptr);
        if (s == INVALID_SOCKET) break;
        SetNonBlocking(s);
        // Nagle off. Collaboration messages are small and latency-sensitive - a lock
        // request delayed 40 ms by coalescing is a visible stall on a click.
        BOOL nd = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd),
                     sizeof(nd));
        const ConnId id = nextId_++;
        Conn c;
        c.sock = static_cast<usize>(s);
        if (Secure()) {
            c.tls = std::make_unique<SecureChannel>();
            c.tls->BeginServer(*id_, policy_);
        } else {
            // Unsecured: the connection IS the peer, so announce it immediately.
            c.announced = true;
            pendingNew_.push_back(id);
        }
        conns_[id] = std::move(c);
    }

    std::vector<ConnId> dead;
    for (auto& [id, c] : conns_) {
        if (!c.open) continue;

        char buf[8192];
        bool closed = false;
        for (;;) {
            const int got = ::recv(S(c.sock), buf, sizeof(buf), 0);
            if (got > 0) {
                const u8* p = reinterpret_cast<u8*>(buf);
                if (c.tls) c.tls->PushCipher(p, static_cast<usize>(got));
                else c.in.insert(c.in.end(), p, p + got);
                continue;
            }
            if (got == 0) { // graceful close by the peer
                closed = true;
                break;
            }
            if (!WouldBlock()) closed = true;
            break;
        }

        // Advance TLS before flushing, so a handshake step that arrived this poll is
        // answered in the same poll. Doing it after the flush would cost an extra tick
        // of latency per round trip, and a TLS handshake is several round trips.
        if (c.tls) {
            c.tls->Pump();
            c.tls->ReceivePlain(c.in);
            std::vector<u8> cipher;
            if (c.tls->PullCipher(cipher))
                c.out.insert(c.out.end(), cipher.begin(), cipher.end());
            if (c.tls->State() == ChannelState::Failed) {
                // Flush the refusal below before dropping, then reap next poll. A peer
                // that is merely unknown deserves to be told why.
                closed = c.out.empty();
            } else if (c.tls->State() == ChannelState::Open && !c.announced) {
                c.announced = true;
                pendingNew_.push_back(id);
            }
        }

        // Flush whatever send() could not take earlier, in order - out-of-order writes
        // corrupt every frame after the first partial send.
        while (!c.out.empty()) {
            const int sent = ::send(S(c.sock), reinterpret_cast<const char*>(c.out.data()),
                                    static_cast<int>(c.out.size()), 0);
            if (sent == SOCKET_ERROR) {
                if (!WouldBlock()) closed = true;
                break;
            }
            if (sent <= 0) break;
            c.out.erase(c.out.begin(), c.out.begin() + sent);
        }
        if (c.tls && c.tls->State() == ChannelState::Failed && c.out.empty()) closed = true;
        if (closed) dead.push_back(id);
    }
    for (const ConnId id : dead) Reap(id);
}

void TcpServerTransport::Reap(ConnId c) {
    const auto it = conns_.find(c);
    if (it == conns_.end() || !it->second.open) return;
    it->second.open = false;
    if (it->second.sock != kInvalid) {
        ::closesocket(S(it->second.sock));
        it->second.sock = kInvalid;
    }
    if (!it->second.announced) {
        // Never handed to the server, so there is nothing to tell it about. Reporting a
        // disconnect for a connection it was never told arrived would leave it removing
        // a peer that does not exist - and every refused stranger would do it.
        conns_.erase(it);
        return;
    }
    pendingGone_.push_back(c);
}

void TcpServerTransport::PollNewConnections(std::vector<ConnId>& out) {
    out = pendingNew_;
    pendingNew_.clear();
}

void TcpServerTransport::PollDisconnects(std::vector<ConnId>& out) {
    out = pendingGone_;
    pendingGone_.clear();
    // Only now is it safe to forget the connection: the server has been told, so it can
    // no longer ask for its bytes. Erasing at close time would drop buffered inbound
    // data the server had not yet drained.
    for (const ConnId c : out) conns_.erase(c);
}

void TcpServerTransport::Receive(ConnId c, std::vector<u8>& out) {
    const auto it = conns_.find(c);
    if (it == conns_.end()) return;
    out.insert(out.end(), it->second.in.begin(), it->second.in.end());
    it->second.in.clear();
}

void TcpServerTransport::Send(ConnId c, const u8* data, usize n) {
    const auto it = conns_.find(c);
    if (it == conns_.end() || !it->second.open || n == 0) return;
    if (it->second.tls) {
        // Encrypt now; Poll() picks the ciphertext up and flushes it.
        it->second.tls->SendPlain(data, n);
        return;
    }
    // Always append to the pending buffer and let Poll() drain it. Sending directly
    // here would have to handle a partial send at every call site.
    it->second.out.insert(it->second.out.end(), data, data + n);
}

void TcpServerTransport::Disconnect(ConnId c) { Reap(c); }

// --- client ------------------------------------------------------------------

TcpClientTransport::~TcpClientTransport() { Disconnect(); }

void TcpClientTransport::EnableSecurity(const Identity& me, PeerPolicy expectHost) {
    id_ = &me;
    policy_ = std::move(expectHost);
}

bool TcpClientTransport::Connect(const char* host, u16 port) {
    if (!wsa_.Ok()) return false;

    // Resolve a NAME, not just a dotted quad. The whole point of this pass is reaching a
    // peer across the internet, and nobody types the IPv4 address of their colleague's
    // machine. inet_pton alone would have made every hostname fail as "connect refused".
    char portText[8];
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // IPv6 too - a lot of home connections are v6-only now
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    if (::getaddrinfo(host, portText, &hints, &results) != 0 || !results) return false;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = results; a; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != SOCKET_ERROR) break;
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(results);
    if (s == INVALID_SOCKET) return false;

    BOOL nd = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    SetNonBlocking(s); // only AFTER connect, so the connect itself stays simple
    sock_ = static_cast<usize>(s);
    open_ = true;
    if (Secure()) {
        tls_ = std::make_unique<SecureChannel>();
        if (!tls_->BeginClient(*id_, policy_)) {
            Disconnect();
            return false;
        }
        // Produce the ClientHello now so the first Poll has something to send.
        tls_->Pump();
        std::vector<u8> cipher;
        if (tls_->PullCipher(cipher)) out_.insert(out_.end(), cipher.begin(), cipher.end());
    }
    return true;
}

bool TcpClientTransport::Handshaking() const {
    return open_ && tls_ && tls_->State() != ChannelState::Open;
}

const char* TcpClientTransport::SecurityError() const {
    return tls_ ? tls_->Error() : "";
}

bool TcpClientTransport::PeerKey(PublicKey& out) const {
    if (!tls_ || tls_->State() != ChannelState::Open) return false;
    out = tls_->PeerKey();
    return true;
}

void TcpClientTransport::Poll() {
    if (!open_) return;
    while (!out_.empty()) {
        const int sent = ::send(S(sock_), reinterpret_cast<const char*>(out_.data()),
                                static_cast<int>(out_.size()), 0);
        if (sent == SOCKET_ERROR) {
            if (!WouldBlock()) Disconnect();
            break;
        }
        if (sent <= 0) break;
        out_.erase(out_.begin(), out_.begin() + sent);
    }
    if (!open_) return;
    char buf[8192];
    bool closed = false;
    for (;;) {
        const int got = ::recv(S(sock_), buf, sizeof(buf), 0);
        if (got > 0) {
            const u8* p = reinterpret_cast<u8*>(buf);
            if (tls_) tls_->PushCipher(p, static_cast<usize>(got));
            else in_.insert(in_.end(), p, p + got);
            continue;
        }
        if (got == 0) {
            closed = true;
            break;
        }
        if (!WouldBlock()) closed = true;
        break;
    }
    if (tls_) {
        tls_->Pump();
        tls_->ReceivePlain(in_);
        std::vector<u8> cipher;
        if (tls_->PullCipher(cipher)) out_.insert(out_.end(), cipher.begin(), cipher.end());
        // Flush again: a handshake step produced this poll would otherwise wait a whole
        // tick, and there are several of them.
        while (!out_.empty()) {
            const int sent = ::send(S(sock_), reinterpret_cast<const char*>(out_.data()),
                                    static_cast<int>(out_.size()), 0);
            if (sent == SOCKET_ERROR) {
                if (!WouldBlock()) closed = true;
                break;
            }
            if (sent <= 0) break;
            out_.erase(out_.begin(), out_.begin() + sent);
        }
        if (tls_->State() == ChannelState::Failed) closed = true;
    }
    if (closed) Disconnect();
}

// On a secured client this is false until BOTH ends have authenticated. "Connected"
// must not mean "a socket exists" here: the caller uses it to decide when the project is
// live, and a socket to an unverified stranger is not that.
bool TcpClientTransport::Connected() const {
    if (!open_) return false;
    return tls_ ? tls_->State() == ChannelState::Open : true;
}

void TcpClientTransport::Receive(std::vector<u8>& out) {
    out.insert(out.end(), in_.begin(), in_.end());
    in_.clear();
}

void TcpClientTransport::Send(const u8* data, usize n) {
    if (!open_ || n == 0) return;
    if (tls_) {
        // Held until the handshake finishes, then flushed. Writing straight to out_
        // would put project bytes on the wire in the clear, ahead of the TLS records.
        tls_->SendPlain(data, n);
        return;
    }
    out_.insert(out_.end(), data, data + n);
}

void TcpClientTransport::Disconnect() {
    if (sock_ != kInvalid) ::closesocket(S(sock_));
    sock_ = kInvalid;
    open_ = false;
    // tls_ is kept deliberately: SecurityError() is most wanted right after the drop.
}

// --- `--test-tcp` -------------------------------------------------------------

namespace {
int g_tcpFails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_tcpFails;
    std::printf("tcp FAIL: %s\n", what);
}
} // namespace

bool TcpTransportSelfTest() {
    g_tcpFails = 0;

    TcpServerTransport st;
    // Port 0 = let the OS choose. A hardcoded port makes this test fail on any machine
    // where something else already owns it - a classic "works on my box" test.
    Check(st.Listen(0), "the server could not listen on an ephemeral localhost port");
    if (!st.Listening()) {
        std::printf("tcp SKIP (no listening socket available)\n");
        return g_tcpFails == 0;
    }
    const u16 port = st.BoundPort();
    Check(port != 0, "the OS did not report a bound port");

    CollabServer server(&st);
    TcpClientTransport ta, tb;
    Check(ta.Connect("127.0.0.1", port), "client A could not connect");
    Check(tb.Connect("127.0.0.1", port), "client B could not connect");

    int paintsB = 0;
    ClientCallbacks ca;
    ClientCallbacks cbk;
    cbk.onPaint = [&](const MsgPaintCommitted&) { ++paintsB; };
    CollabClient A(&ta, ca);
    CollabClient B(&tb, cbk);

    u64 now = 1000;
    // One tick: clients push -> sockets flush -> server processes -> sockets flush ->
    // clients drain. Repeated because a real socket delivers when it feels like it.
    const auto tick = [&](int times, u64 stepMs = 5) {
        for (int i = 0; i < times; ++i) {
            now += stepMs;
            A.Pump(now);
            B.Pump(now);
            ta.Poll();
            tb.Poll();
            st.Poll();
            server.Tick(now);
            st.Poll();
            ta.Poll();
            tb.Poll();
            A.Pump(now);
            B.Pump(now);
        }
    };

    A.Hello("ana");
    B.Hello("ben");
    tick(6);
    Check(A.Ready(), "client A never received its Welcome over TCP");
    Check(B.Ready(), "client B never received its Welcome over TCP");
    Check(A.User() != B.User(), "two TCP clients collapsed onto one UserId");
    Check(server.PeerCount() == 2, "the server did not see two TCP peers");

    // The same mutual-exclusion invariant, now over a real socket.
    EntityKey k;
    k.doc = 1;
    k.guid = 500;
    A.RequestLock(k);
    B.RequestLock(k);
    tick(6);
    const UserId owner = server.LockOf(k).owner;
    Check(owner != 0, "nobody won the lock over TCP");

    A.SendDelta(k, "transform", "{\"a\":1}");
    B.SendDelta(k, "transform", "{\"b\":2}");
    tick(6);
    Check(server.RevisionOf(k) == 1, "both TCP clients wrote the same entity");
    const std::string* val = server.ComponentState(k, "transform");
    Check(val != nullptr, "no value reached the server over TCP");
    if (val)
        Check(*val == (owner == A.User() ? "{\"a\":1}" : "{\"b\":2}"),
              "the lock loser's value won over TCP");

    // A LARGE payload, deliberately bigger than one recv() buffer (8 KiB), so the frame
    // is guaranteed to be split across multiple reads. This is the case the loopback
    // could never exercise and the reason this test exists.
    {
        std::vector<u8> big(64 * 1024);
        for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 31u);
        A.SendPaintOp(4242, 1, big);
        tick(12);
        const std::vector<MsgPaintCommitted>& log = server.PaintHistory(4242);
        Check(log.size() == 1, "a 64 KiB paint op did not arrive as exactly one op");
        if (log.size() == 1)
            Check(log[0].strokeBlob == big,
                  "a 64 KiB stroke blob was corrupted across multiple recv() calls");
        Check(paintsB >= 1, "B never received the broadcast paint op over TCP");
    }

    // MANY SMALL MESSAGES back to back, which TCP will coalesce into single reads -
    // the other half of the framing contract.
    {
        const usize before = server.PaintHistory(99).size();
        for (int i = 0; i < 200; ++i) A.SendPaintOp(99, 1, {static_cast<u8>(i)});
        tick(12);
        Check(server.PaintHistory(99).size() == before + 200,
              "200 coalesced small ops did not all arrive intact");
    }

    // A disconnect must be observed, not hang.
    tb.Disconnect();
    tick(6);
    Check(server.PeerCount() == 1, "the server did not notice a TCP peer leaving");

    // --- the same thing again, but over the open internet's threat model -------------
    //
    // Everything above assumed a trusted LAN. This half asserts that with security on,
    // the session still works AND the three ways a stranger gets in are all shut.
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "hbe_tcpsecure_test";
        std::filesystem::remove_all(dir, ec);
        Identity hostId, peerId, strangerId;
        Check(hostId.LoadOrCreate(dir / "host.hbkey"), "host key");
        Check(peerId.LoadOrCreate(dir / "peer.hbkey"), "peer key");
        Check(strangerId.LoadOrCreate(dir / "stranger.hbkey"), "stranger key");

        Allowlist allow;
        allow.Add(peerId.Public(), "ana");

        TcpServerTransport sec;
        // THE GUARD THAT MATTERS MOST: a public listener without authentication would
        // hand write access to the project to anyone who found the port.
        Check(!sec.BindPublic(41999),
              "BindPublic must REFUSE on an unsecured transport - an open port with no "
              "authentication is a project anyone can edit");
        Check(!sec.Listening(), "the refused BindPublic must not have opened a socket");

        sec.EnableSecurity(hostId, [&allow](const PublicKey& k) { return allow.Allows(k); });
        Check(sec.Listen(0), "the secured server could not listen");
        const u16 sport = sec.BoundPort();
        CollabServer ssrv(&sec);

        const PublicKey hostKey = hostId.Public();
        const PeerPolicy expectHost = [&hostKey](const PublicKey& k) { return k == hostKey; };

        TcpClientTransport good, bad, plain;
        good.EnableSecurity(peerId, expectHost);
        bad.EnableSecurity(strangerId, expectHost);
        Check(good.Connect("127.0.0.1", sport), "the allowed peer could not connect");
        Check(bad.Connect("127.0.0.1", sport), "the stranger could not open a socket");
        // No EnableSecurity at all: a peer that skips TLS and speaks the plain protocol.
        Check(plain.Connect("127.0.0.1", sport), "the plaintext peer could not open a socket");

        ClientCallbacks none;
        CollabClient G(&good, none), P(&plain, none);
        u64 t = 5000;
        // Sampled EVERY tick, not just at the end. A stranger's socket that becomes a
        // server-side peer for a few ticks and is then reaped leaves no trace in a final
        // count, but in the editor it is a name flashing in the user list and a peer slot
        // any passer-by can make the host allocate.
        usize maxPeers = 0;
        const auto stick = [&](int times) {
            for (int i = 0; i < times; ++i) {
                t += 5;
                G.Pump(t);
                P.Pump(t);
                good.Poll();
                bad.Poll();
                plain.Poll();
                sec.Poll();
                ssrv.Tick(t);
                sec.Poll();
                good.Poll();
                bad.Poll();
                plain.Poll();
                G.Pump(t);
                P.Pump(t);
                if (ssrv.PeerCount() > maxPeers) maxPeers = ssrv.PeerCount();
            }
        };

        G.Hello("ana");
        P.Hello("mallory");
        stick(20);

        Check(good.Connected(), "the allowed peer never completed the secure handshake");
        if (!good.Connected()) std::printf("  reason: %s\n", good.SecurityError());
        Check(G.Ready(), "a full collaboration session must still work over TLS");
        PublicKey saw{};
        Check(good.PeerKey(saw) && saw == hostId.Public(),
              "the client must PROVE which host it reached, not assume it");

        // All three intruders, none of which may ever become a peer.
        Check(!bad.Connected(),
              "a peer with valid crypto that is not on the allowlist must not connect");
        Check(!plain.Connected(),
              "a peer that skips TLS entirely must not connect to a secured server");
        Check(!P.Ready(), "a plaintext peer must never receive a Welcome");
        Check(ssrv.PeerCount() == 1,
              "ONLY the allowed peer may become a session on the server");

        // The refused sockets must also not have been reported as peers arriving and
        // then leaving - a phantom join/leave desyncs every user list in the editor.
        stick(10);
        Check(ssrv.PeerCount() == 1,
              "the refused connections must not have churned the server's peer list");
        Check(maxPeers == 1,
              "the server must NEVER have seen more than the one authenticated peer - "
              "an unauthenticated socket must not become a peer even briefly");

        std::filesystem::remove_all(dir, ec);
    }

    if (g_tcpFails == 0) {
        std::printf("tcp: real localhost session - welcome, lock exclusion, delta "
                    "ordering, a 64 KiB frame split across reads, 200 coalesced frames, "
                    "and a clean disconnect; then the same over TLS, where BindPublic "
                    "refuses without security and an unlisted peer, a stranger and a "
                    "plaintext client all fail to become a session\n");
    }
    return g_tcpFails == 0;
}

} // namespace hbe::collab
