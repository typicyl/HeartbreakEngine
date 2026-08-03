// Collab/SecureChannel.h - an encrypted, mutually authenticated stream over the
// open internet.
//
// WHAT WAS WRONG. TcpTransport is plaintext. On a LAN that is a defensible choice; on
// the internet it means anyone on the path - a coffee-shop AP, a compromised router, a
// hostile ISP - can read every scene delta and, worse, REWRITE one. Identity.h fixed
// "who is this peer". It does not help at all if an attacker sits in the middle and
// relays a genuine peer's messages while altering them: both ends would authenticate
// each other perfectly and still be talking through a stranger.
//
// THE SHAPE. TLS 1.3 for the channel, our own ECDSA identity for the peers, and a
// CHANNEL BINDING tying the two together so neither can be attacked separately:
//
//   1. Both ends generate a throwaway self-signed certificate at startup. It carries no
//      trust and is never validated - it exists only to carry the TLS key agreement.
//      There is no PKI, no certificate authority, and nothing to buy or renew.
//   2. TLS 1.3 completes. The stream is now confidential and tamper-evident, but
//      against an UNKNOWN party: a man in the middle would have completed it too.
//   3. Each side sends its long-term public key (Identity.h) and a fresh nonce, then
//      signs a transcript containing BOTH certificate hashes and BOTH nonces.
//
// Step 3 is what makes step 1's untrusted certificate safe. A man in the middle has to
// present its own certificate to each side, so the two ends see DIFFERENT certificate
// hashes; the transcript each signs no longer matches the transcript the other verifies,
// and both drop. It cannot forward the signatures either, because it cannot produce a
// signature over the hash of a certificate it does not hold the key for.
//
// The signed transcript also includes a ROLE BYTE. Without it, a proof captured from
// the client could be replayed back at the client as the server's proof - the classic
// reflection attack, which is easy to miss because everything still "works" in testing.
//
// WHY MBED TLS AND NOT HAND-WRITTEN. Nearly everything in this engine is worth writing
// by hand. A TLS handshake is not: its bugs are silent (a compromised session looks
// exactly like a healthy one), they are found by cryptographers rather than by playing
// the game, and the reviewed implementation IS the value. What is written by hand here
// is only the part that is specific to this engine - the identity binding above.
//
// LAYERING: Core/Types.h, Collab/Identity.h, Mbed TLS. No engine, no sockets. This owns
// no file descriptor and does no I/O: the caller feeds it ciphertext that arrived and
// drains ciphertext to send, which is what lets the existing non-blocking polled
// TcpTransport keep owning the socket, and what lets --test-securechannel run the whole
// handshake with no network at all.
#pragma once

#include "Collab/Identity.h"

#include <functional>
#include <string>
#include <vector>

namespace hbe::collab {

enum class ChannelState : u8 {
    Handshaking,    // TLS is still negotiating
    Authenticating, // TLS is up; proving who holds which key
    // Encrypted, mutually authenticated, and mutually ACCEPTED - both ends ran their
    // own policy and said yes. Open is deliberately a shared fact rather than a local
    // opinion: if each side decided alone, a peer the host had already refused would
    // sit there believing it was connected, queueing work into a session that no longer
    // existed, and the refusal would arrive as unparseable application data.
    Open,
    Failed,
};

// Decides whether a peer that has PROVEN it holds `key` may proceed.
//
// Returning false is a hard drop. A null policy denies everyone: this is the last gate
// before a stranger can write to a project, and the failure mode of defaulting to
// "allow" is that everything works perfectly right up until it matters.
using PeerPolicy = std::function<bool(const PublicKey& key)>;

class SecureChannel {
public:
    SecureChannel();
    ~SecureChannel();
    // Mbed TLS contexts store pointers into themselves (the BIO context is `this`), so
    // moving one would leave the library holding a dangling pointer - as a compile
    // error rather than a crash under load.
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;
    SecureChannel(SecureChannel&&) = delete;
    SecureChannel& operator=(SecureChannel&&) = delete;

    // `me` must outlive the channel; its private key signs the transcript.
    bool BeginServer(const Identity& me, PeerPolicy policy);
    bool BeginClient(const Identity& me, PeerPolicy policy);

    // --- wire side (the caller's socket) ---
    void PushCipher(const u8* data, usize n); // bytes that arrived
    // Moves out everything queued for the wire. Returns false if there was nothing.
    bool PullCipher(std::vector<u8>& out);

    // Advances the handshake and decrypts. Call once per poll, after PushCipher.
    void Pump();

    // --- application side ---
    // Safe to call before Open; bytes are held and flushed once the peer is
    // authenticated. Sending them earlier would put project data on the wire before
    // anyone had proven who they are.
    void SendPlain(const u8* data, usize n);
    void ReceivePlain(std::vector<u8>& out);

    ChannelState State() const { return state_; }
    bool Open_() const { return state_ == ChannelState::Open; }
    const char* Error() const { return err_.c_str(); }

    // Valid once State() == Open: the key the peer PROVED it holds, not one it claimed.
    const PublicKey& PeerKey() const { return peerKey_; }

private:
    bool Begin(bool server, const Identity& me, PeerPolicy policy);
    void Fail(const char* what, int code = 0);
    void PumpAuth();
    bool BuildTranscript(bool forPeer, std::vector<u8>& out) const;
    void QueueAuthFrame(u8 tag, const u8* data, usize n);

    static int BioSend(void* ctx, const unsigned char* buf, usize len);
    static int BioRecv(void* ctx, unsigned char* buf, usize len);

    struct Impl;
    Impl* impl_ = nullptr; // Mbed TLS contexts, kept out of this header

    const Identity* me_ = nullptr;
    PeerPolicy policy_;
    ChannelState state_ = ChannelState::Handshaking;
    bool server_ = false;
    std::string err_;

    std::vector<u8> cipherIn_;  // arrived from the wire, not yet consumed by TLS
    usize cipherInPos_ = 0;
    std::vector<u8> cipherOut_; // produced by TLS, not yet written to the wire
    std::vector<u8> rawIn_;     // decrypted, not yet parsed (auth frames, then app data)
    std::vector<u8> plainIn_;   // decrypted application bytes for the caller
    std::vector<u8> plainOutPending_; // written before Open; flushed on Open

    std::array<u8, 32> myCertHash_{}, peerCertHash_{};
    Challenge myNonce_{}, peerNonce_{};
    PublicKey peerKey_{};
    bool sentProof_ = false, gotPeerHello_ = false, gotPeerProof_ = false;
    bool decided_ = false, sentAccept_ = false, gotPeerAccept_ = false;
};

// --test-securechannel: drives a full server/client handshake with no sockets, then
// asserts every way it must FAIL - a peer not on the allowlist, a tampered ciphertext
// byte, a forged signature, and a man in the middle who completes TLS with both ends.
bool SecureChannelSelfTest();

} // namespace hbe::collab
