// Collab/WebRtcTransport.h - reaching a colleague's machine across the internet.
//
// THE PROBLEM THIS SOLVES, AND ONLY THIS. Identity and encryption were already done
// (Identity.h, SecureChannel.h), and neither of them opens a port. A machine on a home
// connection sits behind NAT: it can dial out, but nothing can dial in. Two such machines
// cannot reach each other at all, no matter how well authenticated they would be.
//
// ICE fixes that by having BOTH sides dial out at the same moment. Each learns its own
// public address from a STUN server (a free, stateless, public one - not ours), the two
// exchange addresses, and the simultaneous outbound packets punch a path through both
// NATs. The result is a DIRECT peer-to-peer link. Project data never touches a third
// party. Where a network genuinely forbids direct connections - some corporate and mobile
// networks - ICE falls back to relaying through a TURN server, which is why TURN is
// configurable and empty by default: a fallback, not the path.
//
// WEBRTC IS USED AS A TRANSPORT AND NOTHING ELSE. It brings its own DTLS, and we do not
// rely on it for identity: SecureChannel runs INSIDE the data channel exactly as it runs
// inside a TCP socket, so the peer is authenticated end to end by its long-term key. That
// is what makes the signalling exchange (Signaling.h) safe to do by copy-paste through
// any channel at all - it carries no authority, so nothing is lost if it is observed or
// tampered with.
//
// EDITOR ONLY. The shipped runtime does not link this; a game has no reason to carry an
// ICE/DTLS/SCTP stack or its threads. See HBE_COLLAB_P2P_SOURCES in CMakeLists.txt.
//
// THREADING. libdatachannel runs its own threads and delivers messages from them. Every
// one of those callbacks does nothing but append to a mutex-guarded buffer; Poll() drains
// them on the engine thread, and everything above this file - locks, revisions, the
// registry - stays single-threaded exactly as before. The alternative, letting a network
// thread touch the scene, would turn every ordering bug in the collaboration layer into a
// genuine data race.
#pragma once

#include "Collab/Identity.h"
#include "Collab/SecureChannel.h"
#include "Collab/Signaling.h"
#include "Collab/Transport.h"

#include <memory>
#include <string>
#include <vector>

namespace hbe::collab {

struct IceConfig {
    // "stun:host:port" or "turn:user:password@host:port".
    std::vector<std::string> servers;

    // Public STUN only. STUN servers never see project data - a peer asks one "what
    // address do my packets appear to come from?" and that is the entire exchange - so
    // using someone else's costs nothing and saves running one.
    static IceConfig Default();
};

enum class LinkState : u8 {
    Gathering,      // working out our own addresses; the invitation is not ready yet
    WaitingForPeer, // invitation ready, nobody has replied
    Connecting,     // reply accepted; punching through, then authenticating
    Open,           // direct link up AND the peer has proven who it is
    Failed,
    Closed,
};

const char* LinkStateName(LinkState s);

// The host. One invitation per guest - WebRTC links are point to point, so a host with
// three collaborators holds three of them.
class WebRtcServerTransport final : public IServerTransport {
public:
    WebRtcServerTransport();
    ~WebRtcServerTransport() override;
    WebRtcServerTransport(const WebRtcServerTransport&) = delete;
    WebRtcServerTransport& operator=(const WebRtcServerTransport&) = delete;

    // REQUIRED before CreateInvitation. Unlike the LAN transport there is no
    // "unsecured, it's just localhost" case here: this exists to be reachable from the
    // internet, so an unauthenticated link would have no legitimate use.
    void EnableSecurity(const Identity& me, PeerPolicy policy);
    void SetIceConfig(const IceConfig& cfg);

    // Starts gathering for one guest. Returns 0 if security was not enabled.
    ConnId CreateInvitation();
    LinkState StateOf(ConnId c) const;
    // The text to send the guest, once StateOf() is WaitingForPeer. Empty before then.
    std::string Invitation(ConnId c) const;
    // The guest's reply, pasted back. The caller says WHICH invitation it answers,
    // because a reply carries nothing that identifies the invitation it came from and
    // guessing would silently cross two guests' sessions.
    bool AcceptReply(ConnId c, const std::string& replyText);

    // The key this peer PROVED it holds. This, not any name from the wire, is who did
    // the work.
    bool PeerKeyOf(ConnId c, PublicKey& out) const;
    // The key a pending invitation's guest CLAIMED in its reply - advisory, for the
    // "add this fingerprint?" prompt. See Signaling.h.
    bool ClaimedKeyOf(ConnId c, PublicKey& out) const;

    // Once per frame, before CollabServer::Tick.
    void Poll();

    // The proven key, through the generic interface - this is what lets CollabServer
    // key a UserId on WHO a peer is rather than on the name it typed.
    bool PeerKey(ConnId c, std::array<u8, 64>& out) const override;

    void PollNewConnections(std::vector<ConnId>& out) override;
    void PollDisconnects(std::vector<ConnId>& out) override;
    void Receive(ConnId c, std::vector<u8>& out) override;
    void Send(ConnId c, const u8* data, usize n) override;
    void Disconnect(ConnId c) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The guest.
class WebRtcClientTransport final : public IClientTransport {
public:
    WebRtcClientTransport();
    ~WebRtcClientTransport() override;
    WebRtcClientTransport(const WebRtcClientTransport&) = delete;
    WebRtcClientTransport& operator=(const WebRtcClientTransport&) = delete;

    void EnableSecurity(const Identity& me, PeerPolicy expectHost);
    void SetIceConfig(const IceConfig& cfg);

    // Consumes the host's invitation and begins gathering our own addresses.
    bool BeginFromInvitation(const std::string& invitationText);
    LinkState State() const;
    // The text to send back, once State() is WaitingForPeer.
    std::string Reply() const;
    // Who the invitation CLAIMED to be from - advisory until the tunnel authenticates.
    bool ClaimedHostKey(PublicKey& out) const;
    // The host key this client PROVED it reached.
    bool PeerKey(PublicKey& out) const;
    const char* Error() const;

    void Poll();

    bool Connected() const override; // true only once authenticated, never merely linked
    void Receive(std::vector<u8>& out) override;
    void Send(const u8* data, usize n) override;
    void Disconnect() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --test-webrtc: a real host and guest in one process, connected over actual ICE and a
// real data channel - blob exchange, direct link, then a full collaboration session
// through it - plus the refusals that must still hold once the door is on the internet.
bool WebRtcSelfTest();

// --net-check: asks the real STUN servers what this machine looks like from outside and
// reports whether direct peer-to-peer is likely to work here.
//
// Separate from the self-test on purpose. This one needs the internet, so it must never
// be something CI or a developer on a train can fail; and it is a DIAGNOSTIC - the thing
// to run when someone says "it won't connect" - rather than an assertion about our code.
bool NetCheck();

} // namespace hbe::collab
