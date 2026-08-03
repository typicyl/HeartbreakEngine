// Collab/WebRtcTransport.cpp
#include "Collab/WebRtcTransport.h"

#include "Collab/CollabClient.h"
#ifndef HBE_COLLAB_JOIN_ONLY
#  include "Collab/CollabServer.h"
#endif

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace hbe::collab {

namespace {

// One SCTP message per chunk. TLS records are at most ~16 KiB, but PullCipher can return
// several at once (a 64 KiB paint op is five of them), and a data channel has a
// negotiated maximum message size. Chunking is safe because the far side feeds the bytes
// into a STREAM parser and SCTP delivers reliably and in order.
constexpr usize kChunk = 16 * 1024;

bool g_verbose = false;

void EnsureLogger() {
    static const bool once = [] {
        // Errors only. libdatachannel is chatty at Info and this shares a console with
        // the engine's own log.
        // Verbose prints EVERY ICE candidate pair, every connectivity check and the
        // reason the agent gives up - which is the only thing that answers "both ends see
        // STUN, so why can't they reach each other". Off by default because it is a
        // torrent and shares the console with the engine log.
        rtc::InitLogger(g_verbose ? rtc::LogLevel::Verbose : rtc::LogLevel::Error);
        return true;
    }();
    (void)once;
}

// Everything one peer-to-peer link needs. The mutex guards only what libdatachannel's
// threads touch; `tls`, `plain` and `state` belong to the engine thread alone.
struct Link {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::unique_ptr<SecureChannel> tls;

    std::mutex m;
    std::vector<u8> inbox; // ciphertext delivered by a network thread
    std::atomic<bool> gathered{false};
    std::atomic<bool> channelOpen{false};
    std::atomic<bool> broken{false};

    std::vector<u8> plain; // decrypted, engine thread only
    LinkState state = LinkState::Gathering;
    std::string blobText;  // our invitation or reply, once gathering finishes
    PublicKey claimed{};
    bool announced = false;
    std::string err;
};

void BindChannel(Link& L, std::shared_ptr<rtc::DataChannel> dc) {
    L.dc = dc;
    dc->onOpen([&L]() { L.channelOpen = true; });
    dc->onClosed([&L]() { L.broken = true; });
    dc->onError([&L](std::string) { L.broken = true; });
    dc->onMessage([&L](rtc::message_variant msg) {
        if (!std::holds_alternative<rtc::binary>(msg)) return; // we only ever send binary
        const rtc::binary& b = std::get<rtc::binary>(msg);
        std::lock_guard<std::mutex> g(L.m);
        const u8* p = reinterpret_cast<const u8*>(b.data());
        L.inbox.insert(L.inbox.end(), p, p + b.size());
    });
}

void WatchPeerConnection(Link& L) {
    L.pc->onGatheringStateChange([&L](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) L.gathered = true;
    });
    L.pc->onStateChange([&L](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Failed ||
            s == rtc::PeerConnection::State::Closed) {
            // RECORD WHY. No callback ever wrote L.err, so Error() was empty for every
            // network-level failure - including the one that actually happens - and the
            // editor's Failed branch printed nothing. A peer that gives up in silence is
            // indistinguishable from one that is still trying.
            if (L.err.empty() && !L.channelOpen) {
                L.err = "the two machines could not reach each other in time. ICE gives "
                        "up after about 40 seconds, and that clock starts when the "
                        "invitation is opened - so the reply has to be pasted back "
                        "within that window.";
            }
            L.broken = true;
        }
    });
}

rtc::Configuration ToRtc(const IceConfig& cfg) {
    rtc::Configuration c;
    for (const std::string& s : cfg.servers) {
        try {
            c.iceServers.emplace_back(s);
        } catch (const std::exception&) {
            // A malformed entry must not take the whole session down; the remaining
            // servers (and plain host candidates) still work.
        }
    }
    return c;
}

// Drives one link's SecureChannel. Engine thread only.
void PumpSecurity(Link& L, bool asServer, const Identity& me, const PeerPolicy& policy) {
    if (!L.channelOpen || L.broken) return;
    if (!L.tls) {
        L.tls = std::make_unique<SecureChannel>();
        const bool ok = asServer ? L.tls->BeginServer(me, policy)
                                 : L.tls->BeginClient(me, policy);
        if (!ok) {
            L.err = L.tls->Error();
            L.broken = true;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> g(L.m);
        if (!L.inbox.empty()) {
            L.tls->PushCipher(L.inbox.data(), L.inbox.size());
            L.inbox.clear();
        }
    }
    L.tls->Pump();
    L.tls->ReceivePlain(L.plain);

    std::vector<u8> cipher;
    if (L.tls->PullCipher(cipher) && L.dc) {
        for (usize off = 0; off < cipher.size(); off += kChunk) {
            const usize n = (cipher.size() - off < kChunk) ? cipher.size() - off : kChunk;
            const auto* p = reinterpret_cast<const std::byte*>(cipher.data() + off);
            try {
                L.dc->send(rtc::binary(p, p + n));
            } catch (const std::exception&) {
                L.broken = true;
                return;
            }
        }
    }
    if (L.tls->State() == ChannelState::Failed) {
        L.err = L.tls->Error();
        // NOT broken yet: the refusal we just queued still has to go out, so the peer
        // learns why instead of seeing a bare disconnect. Poll() closes it next time.
        L.state = LinkState::Failed;
    } else if (L.tls->State() == ChannelState::Open) {
        L.state = LinkState::Open;
    }
}

} // namespace

void SetVerboseLogging(bool on) { g_verbose = on; }

IceConfig IceConfig::Default() {
    IceConfig c;
    // Two providers, so one being down is not an outage. Both are free, public and
    // stateless; neither can see anything but "someone asked for their own address".
    c.servers.push_back("stun:stun.l.google.com:19302");
    c.servers.push_back("stun:stun.cloudflare.com:3478");
    return c;
}

const char* LinkStateName(LinkState s) {
    switch (s) {
    case LinkState::Gathering: return "finding your addresses";
    case LinkState::WaitingForPeer: return "waiting for the other side";
    case LinkState::Connecting: return "connecting";
    case LinkState::Open: return "connected";
    case LinkState::Failed: return "failed";
    case LinkState::Closed: return "closed";
    }
    return "?";
}

// --- host ---------------------------------------------------------------------

struct WebRtcServerTransport::Impl {
    const Identity* id = nullptr;
    PeerPolicy policy;
    IceConfig ice = IceConfig::Default();
    std::unordered_map<ConnId, std::unique_ptr<Link>> links;
    std::vector<ConnId> pendingNew, pendingGone;
    ConnId next = 1;
};

WebRtcServerTransport::WebRtcServerTransport() : impl_(std::make_unique<Impl>()) {
    EnsureLogger();
}
WebRtcServerTransport::~WebRtcServerTransport() = default;

void WebRtcServerTransport::EnableSecurity(const Identity& me, PeerPolicy policy) {
    impl_->id = &me;
    impl_->policy = std::move(policy);
}

void WebRtcServerTransport::SetIceConfig(const IceConfig& cfg) { impl_->ice = cfg; }

ConnId WebRtcServerTransport::CreateInvitation() {
    // No unauthenticated mode, unlike the LAN transport: this link exists to be reachable
    // from the internet, so there is no setting in which an anonymous one is wanted.
    if (!impl_->id) return 0;
    auto L = std::make_unique<Link>();
    try {
        L->pc = std::make_shared<rtc::PeerConnection>(ToRtc(impl_->ice));
    } catch (const std::exception& e) {
        (void)e;
        return 0;
    }
    WatchPeerConnection(*L);
    // Creating the channel is what triggers negotiation, so the offer exists afterwards.
    try {
        BindChannel(*L, L->pc->createDataChannel("hbe-collab"));
    } catch (const std::exception&) {
        return 0;
    }
    const ConnId id = impl_->next++;
    impl_->links[id] = std::move(L);
    return id;
}

LinkState WebRtcServerTransport::StateOf(ConnId c) const {
    const auto it = impl_->links.find(c);
    return it == impl_->links.end() ? LinkState::Closed : it->second->state;
}

std::string WebRtcServerTransport::Invitation(ConnId c) const {
    const auto it = impl_->links.find(c);
    return it == impl_->links.end() ? std::string() : it->second->blobText;
}

bool WebRtcServerTransport::AcceptReply(ConnId c, const std::string& replyText) {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end()) return false;
    Link& L = *it->second;
    // A DEAD LINK MUST NOT BE RESURRECTED. AcceptReply only checked that the ConnId was
    // still in the map, so a link already Closed/Failed was pushed back to Connecting -
    // the host then reported "connecting" against a connection that could never come up.
    if (L.state == LinkState::Closed || L.state == LinkState::Failed) return false;
    SessionBlob blob;
    if (!DecodeSessionBlob(replyText, blob)) return false;
    // An INVITATION pasted where a reply belongs would be set as a remote offer and put
    // the connection into a state neither side can leave. Refuse it with a real answer.
    if (!blob.isAnswer) return false;
    L.claimed = blob.claimedKey;
    try {
        L.pc->setRemoteDescription(rtc::Description(blob.sdp, "answer"));
    } catch (const std::exception& e) {
        L.err = e.what();
        return false;
    }
    L.state = LinkState::Connecting;
    return true;
}

bool WebRtcServerTransport::PeerKeyOf(ConnId c, PublicKey& out) const {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end() || !it->second->tls) return false;
    if (it->second->tls->State() != ChannelState::Open) return false;
    out = it->second->tls->PeerKey();
    return true;
}

bool WebRtcServerTransport::ClaimedKeyOf(ConnId c, PublicKey& out) const {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end()) return false;
    out = it->second->claimed;
    return true;
}

bool WebRtcServerTransport::PeerKey(ConnId c, std::array<u8, 64>& out) const {
    return PeerKeyOf(c, out);
}

void WebRtcServerTransport::Poll() {
    std::vector<ConnId> dead;
    for (auto& [id, up] : impl_->links) {
        Link& L = *up;

        if (L.state == LinkState::Gathering && L.gathered) {
            auto desc = L.pc->localDescription();
            if (desc) {
                SessionBlob b;
                b.isAnswer = false;
                b.claimedKey = impl_->id->Public();
                b.sdp = std::string(desc.value());
                L.blobText = EncodeSessionBlob(b);
                L.state = LinkState::WaitingForPeer;
            }
        }

        if (L.state == LinkState::Connecting || L.state == LinkState::Open ||
            L.state == LinkState::Failed)
            PumpSecurity(L, /*asServer=*/true, *impl_->id, impl_->policy);

        if (L.state == LinkState::Open && !L.announced) {
            L.announced = true;
            impl_->pendingNew.push_back(id);
        }
        if (L.broken || (L.state == LinkState::Failed && L.tls)) dead.push_back(id);
    }

    for (const ConnId id : dead) {
        const auto it = impl_->links.find(id);
        if (it == impl_->links.end()) continue;
        if (it->second->announced) {
            // Only a link the server was told about can be reported as leaving.
            it->second->state = LinkState::Closed;
            impl_->pendingGone.push_back(id);
        } else {
            // A guest that never authenticated was never a peer. Announcing its arrival
            // and departure would flash a stranger through the user list.
            impl_->links.erase(it);
        }
    }
}

void WebRtcServerTransport::PollNewConnections(std::vector<ConnId>& out) {
    out = impl_->pendingNew;
    impl_->pendingNew.clear();
}

void WebRtcServerTransport::PollDisconnects(std::vector<ConnId>& out) {
    out = impl_->pendingGone;
    impl_->pendingGone.clear();
    for (const ConnId c : out) impl_->links.erase(c);
}

void WebRtcServerTransport::Receive(ConnId c, std::vector<u8>& out) {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end()) return;
    Link& L = *it->second;
    if (L.plain.empty()) return;
    out.insert(out.end(), L.plain.begin(), L.plain.end());
    L.plain.clear();
}

void WebRtcServerTransport::Send(ConnId c, const u8* data, usize n) {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end() || n == 0) return;
    if (it->second->tls) it->second->tls->SendPlain(data, n);
}

void WebRtcServerTransport::Disconnect(ConnId c) {
    const auto it = impl_->links.find(c);
    if (it == impl_->links.end()) return;
    it->second->broken = true;
    if (it->second->announced) {
        it->second->state = LinkState::Closed;
        impl_->pendingGone.push_back(c);
    } else {
        impl_->links.erase(it);
    }
}

// --- guest --------------------------------------------------------------------

struct WebRtcClientTransport::Impl {
    const Identity* id = nullptr;
    PeerPolicy policy;
    IceConfig ice = IceConfig::Default();
    Link link;
    PublicKey claimedHost{};
    bool started = false;
};

WebRtcClientTransport::WebRtcClientTransport() : impl_(std::make_unique<Impl>()) {
    EnsureLogger();
}
WebRtcClientTransport::~WebRtcClientTransport() = default;

void WebRtcClientTransport::EnableSecurity(const Identity& me, PeerPolicy expectHost) {
    impl_->id = &me;
    impl_->policy = std::move(expectHost);
}

void WebRtcClientTransport::SetIceConfig(const IceConfig& cfg) { impl_->ice = cfg; }

bool WebRtcClientTransport::BeginFromInvitation(const std::string& invitationText) {
    if (!impl_->id || impl_->started) return false;
    SessionBlob blob;
    if (!DecodeSessionBlob(invitationText, blob)) return false;
    // A reply pasted in place of an invitation is a common mix-up and produces a
    // connection that hangs forever. Name it instead.
    if (blob.isAnswer) return false;
    impl_->claimedHost = blob.claimedKey;

    Link& L = impl_->link;
    try {
        L.pc = std::make_shared<rtc::PeerConnection>(ToRtc(impl_->ice));
    } catch (const std::exception& e) {
        L.err = e.what();
        return false;
    }
    WatchPeerConnection(L);
    // The host created the channel, so it arrives here rather than being created.
    L.pc->onDataChannel([&L](std::shared_ptr<rtc::DataChannel> dc) { BindChannel(L, dc); });
    try {
        // Setting the remote offer generates our answer automatically.
        L.pc->setRemoteDescription(rtc::Description(blob.sdp, "offer"));
    } catch (const std::exception& e) {
        L.err = e.what();
        L.state = LinkState::Failed;
        return false;
    }
    impl_->started = true;
    return true;
}

LinkState WebRtcClientTransport::State() const { return impl_->link.state; }
std::string WebRtcClientTransport::Reply() const { return impl_->link.blobText; }
const char* WebRtcClientTransport::Error() const { return impl_->link.err.c_str(); }

bool WebRtcClientTransport::ClaimedHostKey(PublicKey& out) const {
    if (!impl_->started) return false;
    out = impl_->claimedHost;
    return true;
}

bool WebRtcClientTransport::PeerKey(PublicKey& out) const {
    const Link& L = impl_->link;
    if (!L.tls || L.tls->State() != ChannelState::Open) return false;
    out = L.tls->PeerKey();
    return true;
}

void WebRtcClientTransport::Poll() {
    Link& L = impl_->link;
    if (!impl_->started) return;

    if (L.state == LinkState::Gathering && L.gathered) {
        auto desc = L.pc->localDescription();
        if (desc) {
            SessionBlob b;
            b.isAnswer = true;
            b.claimedKey = impl_->id->Public();
            b.sdp = std::string(desc.value());
            L.blobText = EncodeSessionBlob(b);
            // Our answer is ready. The link comes up as soon as the host accepts it, so
            // there is nothing further for this side to do but keep polling.
            L.state = LinkState::WaitingForPeer;
        }
    }
    if (L.state == LinkState::WaitingForPeer && L.channelOpen) L.state = LinkState::Connecting;
    if (L.state == LinkState::Connecting || L.state == LinkState::Open ||
        L.state == LinkState::Failed)
        PumpSecurity(L, /*asServer=*/false, *impl_->id, impl_->policy);
    // FAILED, NOT CLOSED. Mapping a broken link to Closed meant CollabSession's Failed
    // branch never fired, so the guest sat on "Reply ready - send it back to them."
    // forever while its connection was already dead underneath.
    if (L.broken && L.state != LinkState::Failed)
        L.state = L.err.empty() ? LinkState::Closed : LinkState::Failed;
}

bool WebRtcClientTransport::Connected() const {
    // Never "the link is up". A direct path to someone who has not proven who they are
    // is not a session, and the caller uses this to decide when the project is live.
    const Link& L = impl_->link;
    return L.state == LinkState::Open && L.tls && L.tls->State() == ChannelState::Open;
}

void WebRtcClientTransport::Receive(std::vector<u8>& out) {
    Link& L = impl_->link;
    if (L.plain.empty()) return;
    out.insert(out.end(), L.plain.begin(), L.plain.end());
    L.plain.clear();
}

void WebRtcClientTransport::Send(const u8* data, usize n) {
    if (n == 0) return;
    // Held by SecureChannel until authentication finishes, then flushed - never written
    // out ahead of the handshake.
    if (impl_->link.tls) impl_->link.tls->SendPlain(data, n);
}

void WebRtcClientTransport::Disconnect() {
    impl_->link.broken = true;
    impl_->link.state = LinkState::Closed;
    if (impl_->link.dc) {
        try {
            impl_->link.dc->close();
        } catch (const std::exception&) {
        }
    }
}

// --- self-test ----------------------------------------------------------------

namespace {

int g_rtcFails = 0;
void Check(bool c, const char* what) {
    if (c) return;
    ++g_rtcFails;
    std::printf("webrtc FAIL: %s\n", what);
}

// ICE gathering and the DTLS handshake happen on libdatachannel's threads and need real
// wall time. Everything here is a bounded wait on a condition, never a fixed sleep, so
// the test is as fast as the machine allows and still fails rather than hangs.
template <typename Fn>
bool WaitUntil(Fn cond, int maxMs, const std::function<void()>& pump) {
    for (int waited = 0; waited < maxMs; waited += 10) {
        pump();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pump();
    return cond();
}

} // namespace

#ifndef HBE_COLLAB_JOIN_ONLY
// --test-staleinvite: THE THING A HUMAN ACTUALLY DOES.
//
// Every other test completes the handshake in milliseconds because both peers live in one
// process. In real use somebody creates an invitation, copies it into a chat window, walks
// to another machine, pastes it, waits for a reply, walks back, and pastes that. Minutes
// pass with the host holding a gathered PeerConnection that has no remote description.
//
// If ICE, DTLS or the UDP socket gives up during that window, the link is already dead by
// the time the reply arrives - which looks exactly like "I pasted the reply and nothing
// happened", with both ends reporting a healthy network.
bool WebRtcStaleInviteTest(int delaySeconds) {
    g_rtcFails = 0;
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "hbe_staleinvite_test";
    std::filesystem::remove_all(dir, ec);
    Identity hostId, guestId;
    Check(hostId.LoadOrCreate(dir / "h.hbkey"), "host key");
    Check(guestId.LoadOrCreate(dir / "g.hbkey"), "guest key");

    Allowlist allow;
    allow.Add(guestId.Public(), "guest");
    const PublicKey hostKey = hostId.Public();

    IceConfig local; // host candidates only - hermetic, and enough to link on one machine

    WebRtcServerTransport host;
    host.EnableSecurity(hostId, [&allow](const PublicKey& k) { return allow.Allows(k); });
    host.SetIceConfig(local);
    WebRtcClientTransport guest;
    guest.EnableSecurity(guestId, [&hostKey](const PublicKey& k) { return k == hostKey; });
    guest.SetIceConfig(local);

    const ConnId c = host.CreateInvitation();
    Check(c != 0, "an invitation should start");
    const auto pump = [&]() {
        host.Poll();
        guest.Poll();
    };
    Check(WaitUntil([&] { return host.StateOf(c) == LinkState::WaitingForPeer; }, 10000, pump),
          "the invitation never became ready");
    const std::string invitation = host.Invitation(c);

    // THE HUMAN DELAY. Polled throughout, exactly as the editor does every frame.
    std::printf("  holding the invitation for %d seconds...\n", delaySeconds);
    for (int i = 0; i < delaySeconds * 10; ++i) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Check(host.StateOf(c) == LinkState::WaitingForPeer,
          "THE INVITATION WENT STALE WHILE NOBODY TOUCHED IT - the host's link died on "
          "its own before any reply arrived, which is exactly the 'I pasted the reply and "
          "nothing happened' report");
    if (host.StateOf(c) != LinkState::WaitingForPeer)
        std::puts(LinkStateName(host.StateOf(c)));

    // Now do what the person does next.
    Check(guest.BeginFromInvitation(invitation), "the guest accepts the invitation");
    Check(WaitUntil([&] { return !guest.Reply().empty(); }, 10000, pump),
          "the reply never became ready");
    // ...and ANOTHER delay while they carry the reply back.
    std::puts("  holding the reply, as a person carrying it would...");
    for (int i = 0; i < delaySeconds * 10; ++i) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Check(host.AcceptReply(c, guest.Reply()), "the host should accept the reply");
    Check(WaitUntil([&] { return guest.Connected(); }, 30000, pump),
          "THE LINK DID NOT COME UP after a realistic human delay, even though it comes "
          "up instantly when the same code runs back to back");
    if (!guest.Connected()) std::puts(guest.Error());

    std::filesystem::remove_all(dir, ec);
    if (g_rtcFails == 0)
        std::puts("staleinvite: an invitation survives being carried between machines and still connects");
    return g_rtcFails == 0;
}
#endif

bool NetCheck() {
    EnsureLogger();
    const IceConfig cfg = IceConfig::Default();
    std::printf("net-check: asking %zu STUN server(s) what this machine looks like from "
                "the outside...\n",
                cfg.servers.size());

    std::mutex m;
    std::vector<std::string> cands;
    std::atomic<bool> done{false};
    std::shared_ptr<rtc::PeerConnection> pc;
    try {
        pc = std::make_shared<rtc::PeerConnection>(ToRtc(cfg));
    } catch (const std::exception& e) {
        std::printf("net-check: could not start ICE: %s\n", e.what());
        return false;
    }
    pc->onLocalCandidate([&](rtc::Candidate c) {
        std::lock_guard<std::mutex> g(m);
        cands.push_back(std::string(c));
    });
    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) done = true;
    });
    pc->createDataChannel("net-check");

    for (int waited = 0; waited < 20000 && !done; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::lock_guard<std::mutex> g(m);
    int host = 0, srflx = 0, relay = 0;
    std::string publicAddr;
    for (const std::string& s : cands) {
        if (s.find("typ host") != std::string::npos) ++host;
        if (s.find("typ srflx") != std::string::npos) {
            ++srflx;
            // "a=candidate:<f> <c> UDP <pri> <ADDRESS> <port> typ srflx ..."
            // Stop after the FOURTH space: that leaves `start` on the address. Stopping
            // after the fifth lands on the port, which prints very convincingly.
            usize p = 0;
            int field = 0;
            usize start = 0;
            for (; p < s.size() && field < 4; ++p)
                if (s[p] == ' ') {
                    ++field;
                    start = p + 1;
                }
            const usize end = s.find(' ', start);
            if (end != std::string::npos && publicAddr.empty())
                publicAddr = s.substr(start, end - start);
        }
        if (s.find("typ relay") != std::string::npos) ++relay;
        std::printf("   %s\n", s.c_str());
    }
    std::printf("net-check: gathering %s - %d local, %d public-via-STUN, %d relayed\n",
                done ? "completed" : "TIMED OUT", host, srflx, relay);
    if (!publicAddr.empty())
        std::printf("net-check: your public address appears to be %s\n", publicAddr.c_str());

    if (srflx > 0) {
        std::printf("net-check: OK - STUN reached, so direct peer-to-peer should work.\n");
    } else if (host > 0) {
        std::printf("net-check: no STUN reply. You can still collaborate on a local "
                    "network, but reaching someone over the internet will likely need a "
                    "TURN server. A firewall blocking outbound UDP is the usual cause.\n");
    } else {
        std::printf("net-check: no usable addresses at all - check that the network is "
                    "up and that UDP is not blocked outright.\n");
    }
    return host > 0;
}

#ifdef HBE_COLLAB_JOIN_ONLY
// The Hub links the CLIENT half of collaboration and nothing else - it joins, it does not
// host. This self-test drives a real CollabServer, so compiling it there would drag the
// authority, the lock table and the paint log into a launcher that will never run them.
bool WebRtcSelfTest() { return true; }
#else
bool WebRtcSelfTest() {
    g_rtcFails = 0;
    if (!SignalingSelfTest()) ++g_rtcFails;

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hbe_webrtc_test";
    std::filesystem::remove_all(dir, ec);
    Identity hostId, guestId, strangerId;
    Check(hostId.LoadOrCreate(dir / "host.hbkey"), "host key");
    Check(guestId.LoadOrCreate(dir / "guest.hbkey"), "guest key");
    Check(strangerId.LoadOrCreate(dir / "stranger.hbkey"), "stranger key");
    if (g_rtcFails) return false;

    Allowlist allow;
    allow.Add(guestId.Public(), "ana");
    const PeerPolicy hostPolicy = [&allow](const PublicKey& k) { return allow.Allows(k); };
    const PublicKey hostKey = hostId.Public();
    const PeerPolicy expectHost = [&hostKey](const PublicKey& k) { return k == hostKey; };

    // NO ICE SERVERS in the test. Host candidates alone connect two peers on one machine,
    // so this stays hermetic and fast - a self-test that needs the internet fails on a
    // train and teaches everyone to ignore it. STUN itself is exercised by --net-check.
    IceConfig local;

    WebRtcServerTransport host;
    host.EnableSecurity(hostId, hostPolicy);
    host.SetIceConfig(local);

    WebRtcClientTransport guest;
    guest.EnableSecurity(guestId, expectHost);
    guest.SetIceConfig(local);

    const ConnId conn = host.CreateInvitation();
    Check(conn != 0, "the host should be able to create an invitation");
    if (conn == 0) return false;

    const auto pumpBoth = [&]() {
        host.Poll();
        guest.Poll();
    };

    Check(WaitUntil([&] { return host.StateOf(conn) == LinkState::WaitingForPeer; }, 8000,
                    pumpBoth),
          "the host's invitation never became ready");
    const std::string invitation = host.Invitation(conn);
    Check(!invitation.empty(), "the invitation text should not be empty");

    // What the guest can tell BEFORE trusting anything: who it claims to be from.
    Check(guest.BeginFromInvitation(invitation), "the guest should accept the invitation");
    PublicKey claimed{};
    Check(guest.ClaimedHostKey(claimed) && claimed == hostId.Public(),
          "the invitation should advertise the host's key for the 'is this Ben?' check");

    Check(WaitUntil([&] { return !guest.Reply().empty(); }, 8000, pumpBoth),
          "the guest's reply never became ready");
    Check(host.AcceptReply(conn, guest.Reply()), "the host should accept the reply");

    Check(WaitUntil([&] { return guest.Connected(); }, 15000, pumpBoth),
          "the peer-to-peer link never opened and authenticated");
    if (!guest.Connected()) std::puts(guest.Error());
    PublicKey proven{};
    Check(guest.PeerKey(proven) && proven == hostId.Public(),
          "the guest must PROVE which host it reached, not take the invitation's word");
    Check(host.PeerKeyOf(conn, proven) && proven == guestId.Public(),
          "the host must record the key the guest PROVED");

    // A real collaboration session, over the real peer-to-peer link.
    {
        CollabServer server(&host);
        ClientCallbacks cb;
        CollabClient A(&guest, cb);
        u64 t = 1000;
        const auto tick = [&]() {
            t += 5;
            A.Pump(t);
            guest.Poll();
            host.Poll();
            server.Tick(t);
            host.Poll();
            guest.Poll();
            A.Pump(t);
        };
        A.Hello("ana");
        Check(WaitUntil([&] { return A.Ready(); }, 5000, tick),
              "a collaboration session must work over the peer-to-peer link");
        Check(server.PeerCount() == 1, "the host should see exactly one peer");

        EntityKey k;
        k.doc = 1;
        k.guid = 77;
        A.RequestLock(k);
        // The `!= 0` matters. Before the session comes up A.User() is 0 and an unheld
        // lock's owner is also 0, so comparing them alone is 0 == 0 - an assertion that
        // passes precisely when nothing works.
        Check(WaitUntil([&] { return server.LockOf(k).owner != 0; }, 5000, tick) &&
                  server.LockOf(k).owner == A.User() && A.User() != 0,
              "a lock request must round-trip over the peer-to-peer link");
        A.SendDelta(k, "transform", "{\"x\":9}");
        Check(WaitUntil([&] { return server.ComponentState(k, "transform") != nullptr; }, 5000,
                        tick),
              "a scene delta must round-trip over the peer-to-peer link");

        // A payload far larger than one SCTP message, to prove the chunking is not
        // reordering or truncating anything.
        std::vector<u8> big(200 * 1024);
        for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 13u);
        A.SendPaintOp(7, 1, big);
        Check(WaitUntil([&] { return server.PaintHistory(7).size() == 1; }, 10000, tick),
              "a 200 KiB paint op did not arrive over the peer-to-peer link");
        if (server.PaintHistory(7).size() == 1)
            Check(server.PaintHistory(7)[0].strokeBlob == big,
                  "a 200 KiB blob was corrupted by data-channel chunking");
    }

    // AN UNINVITED GUEST. Its crypto is valid and it has a genuine invitation - it is
    // simply not on the allowlist, which must be enough.
    {
        WebRtcServerTransport h2;
        h2.EnableSecurity(hostId, hostPolicy);
        h2.SetIceConfig(local);
        WebRtcClientTransport stranger;
        stranger.EnableSecurity(strangerId, expectHost);
        stranger.SetIceConfig(local);

        const ConnId c2 = h2.CreateInvitation();
        const auto pump2 = [&]() {
            h2.Poll();
            stranger.Poll();
        };
        Check(WaitUntil([&] { return h2.StateOf(c2) == LinkState::WaitingForPeer; }, 8000, pump2),
              "second invitation never became ready");
        Check(stranger.BeginFromInvitation(h2.Invitation(c2)), "the stranger takes the invitation");
        Check(WaitUntil([&] { return !stranger.Reply().empty(); }, 8000, pump2),
              "the stranger's reply never became ready");
        Check(h2.AcceptReply(c2, stranger.Reply()), "the reply is well-formed");

        CollabServer s2(&h2);
        u64 t = 1;
        usize maxPeers = 0;
        const auto tick2 = [&]() {
            t += 5;
            stranger.Poll();
            h2.Poll();
            s2.Tick(t);
            h2.Poll();
            stranger.Poll();
            if (s2.PeerCount() > maxPeers) maxPeers = s2.PeerCount();
        };
        WaitUntil([&] { return stranger.State() == LinkState::Failed; }, 12000, tick2);
        Check(!stranger.Connected(),
              "AN UNINVITED PEER MUST NOT CONNECT, even holding a real invitation");
        Check(maxPeers == 0,
              "an unauthenticated peer must never become a session on the host - not even "
              "for one tick");
    }

    // MIXED-UP BLOBS. Both of these otherwise produce a connection that hangs with no
    // diagnosis, which is the worst possible outcome for a copy-paste workflow.
    {
        WebRtcClientTransport g2;
        g2.EnableSecurity(guestId, expectHost);
        g2.SetIceConfig(local);
        Check(!g2.BeginFromInvitation(guest.Reply()),
              "a REPLY pasted where an invitation belongs must be refused, not hang");
        Check(!g2.BeginFromInvitation("not a blob at all"),
              "arbitrary text must be refused");
        Check(!host.AcceptReply(conn, invitation),
              "an INVITATION pasted where a reply belongs must be refused");
    }

    std::filesystem::remove_all(dir, ec);
    if (g_rtcFails == 0) {
        std::printf("webrtc: a real ICE link between two peers - invitation and reply "
                    "exchanged as text, direct data channel, mutual proof of identity, "
                    "then a full collaboration session (welcome, lock, delta, a 200 KiB "
                    "blob) through it; an uninvited peer holding a real invitation never "
                    "becomes a session, and swapped blobs are refused rather than "
                    "hanging\n");
    }
    return g_rtcFails == 0;
}
#endif // HBE_COLLAB_JOIN_ONLY

} // namespace hbe::collab
