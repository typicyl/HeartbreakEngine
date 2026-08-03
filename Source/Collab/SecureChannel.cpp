// Collab/SecureChannel.cpp - TLS 1.3 (Mbed TLS) bound to our own ECDSA identities.
#include "Collab/SecureChannel.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstdio>
#include <cstring>

namespace hbe::collab {

namespace {

constexpr u8 kTagHello = 1;   // public key + nonce
constexpr u8 kTagProof = 2;   // signature over the transcript
// "I know who you are and you are not welcome here." Sent before dropping, because the
// alternative - closing silently - leaves the far end believing it is connected and
// queueing work into a session that no longer exists. It leaks nothing: a peer learns
// exactly the same fact from the disconnect, just without being able to say why.
constexpr u8 kTagRefused = 3;
// "You are welcome here." Both ends must say it before either treats the session as
// open, so a refusal can never arrive after the far end has already started work.
constexpr u8 kTagAccepted = 4;
constexpr usize kMaxAuthFrame = 512;
// Bumping this MUST break compatibility on purpose: it is mixed into every signature,
// so an old peer and a new peer simply fail to authenticate rather than negotiating
// down to whatever the older one understood.
constexpr char kAuthLabel[] = "HBE-COLLAB-AUTH-v1";
constexpr usize kLabelLen = sizeof(kAuthLabel) - 1;

// Process-wide RNG. Mbed TLS's DRBG is not thread-safe and MBEDTLS_THREADING_C is off,
// which is correct here: the whole collaboration layer is single-threaded and polled by
// design (see TcpTransport.h). If that ever changes this needs a lock, so it asserts
// nothing and hides nothing - it is stated instead.
struct Rng {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;
    Rng() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const char kPers[] = "heartbreak-collab";
        ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   reinterpret_cast<const unsigned char*>(kPers),
                                   sizeof(kPers) - 1) == 0;
    }
    ~Rng() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

Rng& TheRng() {
    static Rng r;
    return r;
}

void Sha256Of(const u8* p, usize n, std::array<u8, 32>& out) {
    mbedtls_sha256(p, n, out.data(), 0);
}

// The bytes both ends sign. Every field that could differ between a genuine session and
// an attacked one is in here:
//   * both CERTIFICATE HASHES - a man in the middle must present its own certificate to
//     each side, so its two views differ and neither signature verifies at the far end;
//   * both NONCES - so a recording of yesterday's session cannot be replayed;
//   * the SIGNER'S ROLE - so the client's proof cannot be reflected back as the
//     server's, which is the failure that looks like it works.
std::vector<u8> MakeTranscript(char role, const std::array<u8, 32>& clientCert,
                               const std::array<u8, 32>& serverCert,
                               const Challenge& clientNonce, const Challenge& serverNonce) {
    std::vector<u8> t;
    t.reserve(kLabelLen + 1 + 128);
    t.insert(t.end(), kAuthLabel, kAuthLabel + kLabelLen);
    t.push_back(static_cast<u8>(role));
    t.insert(t.end(), clientCert.begin(), clientCert.end());
    t.insert(t.end(), serverCert.begin(), serverCert.end());
    t.insert(t.end(), clientNonce.begin(), clientNonce.end());
    t.insert(t.end(), serverNonce.begin(), serverNonce.end());
    return t;
}

} // namespace

struct SecureChannel::Impl {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    std::vector<u8> certDer;

    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&key);
    }
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&crt);
        mbedtls_pk_free(&key);
    }
};

namespace {

// A throwaway certificate for the TLS key agreement. It asserts NOTHING - it is never
// validated, its subject name is a constant, and it is regenerated every run. All of a
// peer's actual identity lives in Identity.h; this only has to be a key the TLS layer
// can do ECDHE with, and a thing whose hash both ends can bind their signatures to.
bool MakeSelfSigned(mbedtls_pk_context& key, mbedtls_x509_crt& crt, std::vector<u8>& der) {
    Rng& rng = TheRng();
    if (!rng.ok) return false;
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
        return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &rng.drbg) != 0)
        return false;

    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    bool ok = true;
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &key);
    mbedtls_x509write_crt_set_issuer_key(&w, &key);
    ok = ok && mbedtls_x509write_crt_set_subject_name(&w, "CN=heartbreak-peer") == 0;
    ok = ok && mbedtls_x509write_crt_set_issuer_name(&w, "CN=heartbreak-peer") == 0;
    u8 serial[16] = {1};
    if (ok && mbedtls_ctr_drbg_random(&rng.drbg, serial + 1, sizeof(serial) - 1) != 0)
        ok = false;
    ok = ok && mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial)) == 0;
    // Deliberately fixed and wide. Nothing validates these, and deriving them from the
    // clock would make a peer with a wrong system date unable to connect for a reason
    // no one would ever guess from the error.
    ok = ok && mbedtls_x509write_crt_set_validity(&w, "20240101000000", "20440101000000") == 0;

    if (ok) {
        u8 buf[2048];
        const int n = mbedtls_x509write_crt_der(&w, buf, sizeof(buf),
                                                mbedtls_ctr_drbg_random, &rng.drbg);
        // The DER is written to the END of the buffer and the return value is its
        // length, not an offset. Reading from the front here yields zeros and a
        // certificate that fails to parse for no visible reason.
        if (n > 0) der.assign(buf + sizeof(buf) - static_cast<usize>(n), buf + sizeof(buf));
        else ok = false;
    }
    mbedtls_x509write_crt_free(&w);
    if (!ok) return false;
    return mbedtls_x509_crt_parse_der(&crt, der.data(), der.size()) == 0;
}

} // namespace

SecureChannel::SecureChannel() = default;

SecureChannel::~SecureChannel() {
    delete impl_;
}

void SecureChannel::Fail(const char* what, int code) {
    state_ = ChannelState::Failed;
    if (!err_.empty()) return; // keep the FIRST cause; later ones are consequences
    char detail[128] = {};
    if (code != 0) mbedtls_strerror(code, detail, sizeof(detail));
    err_ = code ? (std::string(what) + ": " + detail) : std::string(what);
}

int SecureChannel::BioSend(void* ctx, const unsigned char* buf, usize len) {
    auto* self = static_cast<SecureChannel*>(ctx);
    self->cipherOut_.insert(self->cipherOut_.end(), buf, buf + len);
    return static_cast<int>(len);
}

int SecureChannel::BioRecv(void* ctx, unsigned char* buf, usize len) {
    auto* self = static_cast<SecureChannel*>(ctx);
    const usize have = self->cipherIn_.size() - self->cipherInPos_;
    if (have == 0) {
        // NOT an error and NOT end-of-stream: the socket simply has nothing yet. Saying
        // anything else here makes Mbed TLS tear down a perfectly healthy connection the
        // first time a poll finds an empty buffer.
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    const usize n = have < len ? have : len;
    std::memcpy(buf, self->cipherIn_.data() + self->cipherInPos_, n);
    self->cipherInPos_ += n;
    if (self->cipherInPos_ == self->cipherIn_.size()) {
        self->cipherIn_.clear();
        self->cipherInPos_ = 0;
    }
    return static_cast<int>(n);
}

bool SecureChannel::Begin(bool server, const Identity& me, PeerPolicy policy) {
    if (impl_) return false;
    if (!me.Valid()) {
        Fail("no local identity");
        return false;
    }
    me_ = &me;
    policy_ = std::move(policy);
    server_ = server;
    if (!MakeChallenge(myNonce_)) {
        Fail("no CSPRNG for the session nonce");
        return false;
    }

    impl_ = new Impl();
    if (!MakeSelfSigned(impl_->key, impl_->crt, impl_->certDer)) {
        Fail("could not generate the session certificate");
        return false;
    }
    Sha256Of(impl_->certDer.data(), impl_->certDer.size(), myCertHash_);

    int r = mbedtls_ssl_config_defaults(&impl_->conf,
                                        server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        Fail("TLS config", r);
        return false;
    }
    // TLS 1.3 ONLY, both ends. We control both binaries, so there is no legacy peer to
    // accommodate and therefore no reason to leave a downgrade path open.
    mbedtls_ssl_conf_min_tls_version(&impl_->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&impl_->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&impl_->conf, mbedtls_ctr_drbg_random, &TheRng().drbg);
    // The certificates carry no trust, so there is nothing for the X.509 layer to check
    // and no CA to check it against. VERIFY_OPTIONAL on the server is what makes it ASK
    // the client for a certificate at all - without a client certificate there would be
    // no client hash to bind the transcript to. Authentication is not being skipped; it
    // happens below, against keys X.509 knows nothing about.
    mbedtls_ssl_conf_authmode(&impl_->conf, server ? MBEDTLS_SSL_VERIFY_OPTIONAL
                                                   : MBEDTLS_SSL_VERIFY_NONE);
    r = mbedtls_ssl_conf_own_cert(&impl_->conf, &impl_->crt, &impl_->key);
    if (r != 0) {
        Fail("TLS own certificate", r);
        return false;
    }
    r = mbedtls_ssl_setup(&impl_->ssl, &impl_->conf);
    if (r != 0) {
        Fail("TLS setup", r);
        return false;
    }
    if (!server) {
        // Not validated, but TLS 1.3 wants a name for SNI purposes.
        mbedtls_ssl_set_hostname(&impl_->ssl, "heartbreak-peer");
    }
    mbedtls_ssl_set_bio(&impl_->ssl, this, &SecureChannel::BioSend, &SecureChannel::BioRecv,
                        nullptr);
    state_ = ChannelState::Handshaking;
    return true;
}

bool SecureChannel::BeginServer(const Identity& me, PeerPolicy policy) {
    return Begin(true, me, std::move(policy));
}
bool SecureChannel::BeginClient(const Identity& me, PeerPolicy policy) {
    return Begin(false, me, std::move(policy));
}

void SecureChannel::PushCipher(const u8* data, usize n) {
    if (n) cipherIn_.insert(cipherIn_.end(), data, data + n);
}

bool SecureChannel::PullCipher(std::vector<u8>& out) {
    if (cipherOut_.empty()) return false;
    out = std::move(cipherOut_);
    cipherOut_.clear();
    return true;
}

bool SecureChannel::BuildTranscript(bool forPeer, std::vector<u8>& out) const {
    const bool signerIsServer = forPeer ? !server_ : server_;
    const char role = signerIsServer ? 'S' : 'C';
    const auto& clientHash = server_ ? peerCertHash_ : myCertHash_;
    const auto& serverHash = server_ ? myCertHash_ : peerCertHash_;
    const auto& clientNonce = server_ ? peerNonce_ : myNonce_;
    const auto& serverNonce = server_ ? myNonce_ : peerNonce_;
    out = MakeTranscript(role, clientHash, serverHash, clientNonce, serverNonce);
    return true;
}

void SecureChannel::QueueAuthFrame(u8 tag, const u8* data, usize n) {
    std::vector<u8> f;
    f.reserve(3 + n);
    f.push_back(tag);
    f.push_back(static_cast<u8>(n & 0xFF));
    f.push_back(static_cast<u8>((n >> 8) & 0xFF));
    if (n) f.insert(f.end(), data, data + n);
    usize sent = 0;
    while (sent < f.size()) {
        const int w = mbedtls_ssl_write(&impl_->ssl, f.data() + sent, f.size() - sent);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) {
            Fail("TLS write", w);
            return;
        }
        sent += static_cast<usize>(w);
    }
}

void SecureChannel::PumpAuth() {
    // Consume whole auth frames from the front of the decrypted stream. Anything left
    // when authentication finishes is application data and stays put.
    for (;;) {
        if (rawIn_.size() < 3) return;
        const u8 tag = rawIn_[0];
        const usize len = static_cast<usize>(rawIn_[1]) | (static_cast<usize>(rawIn_[2]) << 8);
        if (len > kMaxAuthFrame) {
            Fail("oversized auth frame");
            return;
        }
        if (rawIn_.size() < 3 + len) return; // incomplete, wait for more
        const u8* body = rawIn_.data() + 3;

        if (tag == kTagHello) {
            if (gotPeerHello_ || len != sizeof(PublicKey) + sizeof(Challenge)) {
                Fail("malformed or repeated auth hello");
                return;
            }
            std::memcpy(peerKey_.data(), body, sizeof(PublicKey));
            std::memcpy(peerNonce_.data(), body + sizeof(PublicKey), sizeof(Challenge));
            gotPeerHello_ = true;
        } else if (tag == kTagProof) {
            // A proof before the hello would be a signature over a transcript we cannot
            // reconstruct - refuse rather than guess.
            if (!gotPeerHello_ || gotPeerProof_ || len != sizeof(Signature)) {
                Fail("malformed or out-of-order auth proof");
                return;
            }
            Signature sig{};
            std::memcpy(sig.data(), body, sizeof(Signature));
            std::vector<u8> t;
            BuildTranscript(/*forPeer=*/true, t);
            if (!Verify(peerKey_, t.data(), t.size(), sig)) {
                // Either the peer does not hold the key it claimed, or something is
                // sitting between us and its view of the certificates differs from ours.
                Fail("peer failed to prove it holds its key (or the channel is being "
                     "intercepted)");
                return;
            }
            gotPeerProof_ = true;
        } else if (tag == kTagRefused) {
            Fail("the peer refused this identity - ask them to add your fingerprint");
            return;
        } else if (tag == kTagAccepted) {
            if (!gotPeerProof_ || len != 0) {
                // An acceptance before the proof would be an acceptance of nobody.
                Fail("out-of-order acceptance");
                return;
            }
            gotPeerAccept_ = true;
        } else {
            Fail("unknown auth frame");
            return;
        }
        rawIn_.erase(rawIn_.begin(), rawIn_.begin() + static_cast<std::ptrdiff_t>(3 + len));

        // Our own proof can only be built once the peer's nonce is known.
        if (gotPeerHello_ && !sentProof_) {
            std::vector<u8> t;
            BuildTranscript(/*forPeer=*/false, t);
            Signature mine{};
            if (!me_->Sign(t.data(), t.size(), mine)) {
                Fail("could not sign the session transcript");
                return;
            }
            sentProof_ = true;
            QueueAuthFrame(kTagProof, mine.data(), mine.size());
            if (state_ == ChannelState::Failed) return;
        }

        if (gotPeerProof_ && sentProof_ && !decided_) {
            // The peer has proven WHO it is. Whether that is someone this project lets
            // in is a separate question, and a null policy answers "no".
            decided_ = true;
            if (!policy_ || !policy_(peerKey_)) {
                // Tell them BEFORE dropping, or they sit in a session that is already
                // gone. Queued first because Fail() closes the door behind it.
                QueueAuthFrame(kTagRefused, nullptr, 0);
                Fail("peer is not authorised for this project");
                return;
            }
            sentAccept_ = true;
            QueueAuthFrame(kTagAccepted, nullptr, 0);
            if (state_ == ChannelState::Failed) return;
        }

        if (sentAccept_ && gotPeerAccept_) {
            state_ = ChannelState::Open;
            if (!plainOutPending_.empty()) {
                std::vector<u8> pending;
                pending.swap(plainOutPending_);
                SendPlain(pending.data(), pending.size());
            }
            return;
        }
    }
}

void SecureChannel::Pump() {
    if (state_ == ChannelState::Failed || !impl_) return;

    if (state_ == ChannelState::Handshaking) {
        const int r = mbedtls_ssl_handshake(&impl_->ssl);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        if (r != 0) {
            Fail("TLS handshake", r);
            return;
        }
        const mbedtls_x509_crt* pc = mbedtls_ssl_get_peer_cert(&impl_->ssl);
        if (!pc || pc->raw.len == 0) {
            // Without the peer's certificate there is nothing to bind the signatures to,
            // and the transcript would degrade to something a man in the middle could
            // satisfy. Substituting zeros here would make the handshake succeed and the
            // protection silently vanish, so this fails loudly instead.
            Fail("peer presented no certificate - cannot bind the channel");
            return;
        }
        Sha256Of(pc->raw.p, pc->raw.len, peerCertHash_);
        state_ = ChannelState::Authenticating;
        u8 hello[sizeof(PublicKey) + sizeof(Challenge)];
        std::memcpy(hello, me_->Public().data(), sizeof(PublicKey));
        std::memcpy(hello + sizeof(PublicKey), myNonce_.data(), sizeof(Challenge));
        QueueAuthFrame(kTagHello, hello, sizeof(hello));
        if (state_ == ChannelState::Failed) return;
    }

    for (;;) {
        u8 buf[4096];
        const int r = mbedtls_ssl_read(&impl_->ssl, buf, sizeof(buf));
        if (r > 0) {
            rawIn_.insert(rawIn_.end(), buf, buf + r);
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            Fail("peer closed the connection");
            return;
        }
        if (r == 0) break;
        Fail("TLS read", r);
        return;
    }

    if (state_ == ChannelState::Authenticating) PumpAuth();
    if (state_ == ChannelState::Open && !rawIn_.empty()) {
        plainIn_.insert(plainIn_.end(), rawIn_.begin(), rawIn_.end());
        rawIn_.clear();
    }
}

void SecureChannel::SendPlain(const u8* data, usize n) {
    if (!n || state_ == ChannelState::Failed) return;
    if (state_ != ChannelState::Open) {
        // Held, not dropped and not sent early: project bytes must not reach the wire
        // before the far end has proven who it is.
        plainOutPending_.insert(plainOutPending_.end(), data, data + n);
        return;
    }
    usize sent = 0;
    while (sent < n) {
        const int w = mbedtls_ssl_write(&impl_->ssl, data + sent, n - sent);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) {
            Fail("TLS write", w);
            return;
        }
        sent += static_cast<usize>(w);
    }
}

void SecureChannel::ReceivePlain(std::vector<u8>& out) {
    if (plainIn_.empty()) return;
    out.insert(out.end(), plainIn_.begin(), plainIn_.end());
    plainIn_.clear();
}

// --- self-test ---------------------------------------------------------------

namespace {

// Moves whatever each side has produced to the other and lets both advance. This is
// exactly what TcpTransport::Poll will do with a socket in the middle.
void Relay(SecureChannel& a, SecureChannel& b, int rounds = 40) {
    std::vector<u8> buf;
    for (int i = 0; i < rounds; ++i) {
        a.Pump();
        if (a.PullCipher(buf)) b.PushCipher(buf.data(), buf.size());
        b.Pump();
        if (b.PullCipher(buf)) a.PushCipher(buf.data(), buf.size());
        if (a.State() == ChannelState::Failed || b.State() == ChannelState::Failed) return;
    }
}

} // namespace

bool SecureChannelSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("securechannel FAIL: %s\n", what);
    };

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "hbe_securechannel_test";
    std::filesystem::remove_all(dir, ec);

    Identity host, peer, stranger;
    check(host.LoadOrCreate(dir / "host.hbkey"), "host key");
    check(peer.LoadOrCreate(dir / "peer.hbkey"), "peer key");
    check(stranger.LoadOrCreate(dir / "stranger.hbkey"), "stranger key");
    if (fails) return false;

    Allowlist allow;
    allow.Add(peer.Public(), "peer");
    const PeerPolicy hostPolicy = [&allow](const PublicKey& k) { return allow.Allows(k); };
    const PublicKey hostKey = host.Public();
    const PeerPolicy peerPolicy = [&hostKey](const PublicKey& k) { return k == hostKey; };

    // 1. THE HAPPY PATH: both ends open, each learns the OTHER's real key, and
    //    application bytes cross intact.
    {
        SecureChannel s, c;
        check(s.BeginServer(host, hostPolicy), "server should begin");
        check(c.BeginClient(peer, peerPolicy), "client should begin");
        Relay(s, c);
        check(s.State() == ChannelState::Open, "server should reach Open");
        check(c.State() == ChannelState::Open, "client should reach Open");
        if (s.State() != ChannelState::Open) std::printf("  server: %s\n", s.Error());
        if (c.State() != ChannelState::Open) std::printf("  client: %s\n", c.Error());
        check(s.PeerKey() == peer.Public(), "server must learn the client's REAL key");
        check(c.PeerKey() == host.Public(), "client must learn the server's REAL key");

        const char kMsg[] = "scene delta";
        c.SendPlain(reinterpret_cast<const u8*>(kMsg), sizeof(kMsg) - 1);
        Relay(s, c);
        std::vector<u8> got;
        s.ReceivePlain(got);
        check(got.size() == sizeof(kMsg) - 1 &&
                  std::memcmp(got.data(), kMsg, got.size()) == 0,
              "application bytes must survive the channel");
    }

    // 2. NOT ON THE ALLOWLIST. The stranger's crypto is perfectly valid - it really does
    //    hold its key. It still must not get in.
    {
        SecureChannel s, c;
        s.BeginServer(host, hostPolicy);
        c.BeginClient(stranger, peerPolicy);
        Relay(s, c);
        check(s.State() == ChannelState::Failed,
              "a peer with a VALID key that is not on the allowlist must be refused");
        check(c.State() != ChannelState::Open,
              "the refused peer must not believe the session is open");
    }

    // 3. TAMPERING. Flip one byte of ciphertext in flight; TLS's AEAD must reject the
    //    record rather than hand a corrupted scene delta to the engine.
    {
        SecureChannel s, c;
        s.BeginServer(host, hostPolicy);
        c.BeginClient(peer, peerPolicy);
        Relay(s, c);
        check(s.State() == ChannelState::Open && c.State() == ChannelState::Open,
              "the channel should be open before tampering");
        const char kMsg[] = "delete everything";
        c.SendPlain(reinterpret_cast<const u8*>(kMsg), sizeof(kMsg) - 1);
        c.Pump();
        std::vector<u8> wire;
        check(c.PullCipher(wire), "the client should have produced a record");
        if (!wire.empty()) wire[wire.size() / 2] ^= 0x40;
        s.PushCipher(wire.data(), wire.size());
        s.Pump();
        check(s.State() == ChannelState::Failed, "a tampered record must be rejected");
        std::vector<u8> got;
        s.ReceivePlain(got);
        check(got.empty(), "NOTHING from a tampered record may reach the application");
    }

    // 4. CHANNEL BINDING - the man in the middle, modelled at the transcript level.
    //
    //    An interceptor terminates TLS with both ends, so it can forward the genuine
    //    identity frames verbatim and both sides see keys they trust. What it cannot do
    //    is make the two ends agree on a certificate hash: it must present its OWN
    //    certificate to each of them. So the client signs over the interceptor's
    //    certificate while the server verifies over its own, and the proof fails.
    {
        std::array<u8, 32> clientCert{}, serverCert{}, mitmCert{};
        clientCert.fill(0x11);
        serverCert.fill(0x22);
        mitmCert.fill(0x33);
        Challenge nc{}, ns{};
        check(MakeChallenge(nc) && MakeChallenge(ns), "nonces");

        // What the client sees and signs: its own certificate, and the MITM's posing as
        // the server's.
        const std::vector<u8> clientView = MakeTranscript('C', clientCert, mitmCert, nc, ns);
        Signature sig{};
        check(peer.Sign(clientView.data(), clientView.size(), sig), "client signs");
        check(Verify(peer.Public(), clientView.data(), clientView.size(), sig),
              "the client's own view must verify against itself");

        // What the server sees and verifies: the MITM's certificate posing as the
        // client's, and its own real one.
        const std::vector<u8> serverView = MakeTranscript('C', mitmCert, serverCert, nc, ns);
        check(!Verify(peer.Public(), serverView.data(), serverView.size(), sig),
              "MAN IN THE MIDDLE: a proof signed over the interceptor's certificate "
              "must NOT verify against the certificate the far end actually sees");

        // REFLECTION: the client's proof replayed as if it were the server's.
        const std::vector<u8> asServer = MakeTranscript('S', clientCert, mitmCert, nc, ns);
        check(!Verify(peer.Public(), asServer.data(), asServer.size(), sig),
              "REFLECTION: a client's proof must not verify as a server's");

        // REPLAY: yesterday's proof against today's nonces.
        Challenge fresh{};
        MakeChallenge(fresh);
        const std::vector<u8> newSession = MakeTranscript('C', clientCert, mitmCert, fresh, ns);
        check(!Verify(peer.Public(), newSession.data(), newSession.size(), sig),
              "REPLAY: a recorded proof must not verify in a new session");
    }

    // 5. A REFUSED PEER'S WORK NEVER LANDS. The stranger queues a scene edit the instant
    //    it starts connecting, before anyone has authenticated anything. It must not
    //    reach the far application - "refused" has to mean the data was never acted on,
    //    not merely that a badge said unauthorised.
    {
        SecureChannel s, c;
        s.BeginServer(host, hostPolicy);
        c.BeginClient(stranger, peerPolicy);
        const char kEdit[] = "move every entity to the origin";
        c.SendPlain(reinterpret_cast<const u8*>(kEdit), sizeof(kEdit) - 1);
        Relay(s, c);
        check(s.State() == ChannelState::Failed, "the stranger must be refused");
        std::vector<u8> got;
        s.ReceivePlain(got);
        check(got.empty(), "a REFUSED peer's queued edit must never reach the application");
        check(s.PeerKey() == stranger.Public(),
              "the server must record the key the peer PROVED, so the host can add the "
              "right fingerprint if this was a colleague rather than an intruder");
    }

    std::filesystem::remove_all(dir, ec);
    if (fails == 0) {
        std::printf("securechannel: TLS 1.3 opens and carries application bytes; an "
                    "unlisted peer with a valid key is refused; a tampered record is "
                    "rejected with nothing reaching the application; and an intercepted, "
                    "reflected or replayed proof all fail to verify\n");
    }
    return fails == 0;
}

} // namespace hbe::collab
