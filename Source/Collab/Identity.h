// Collab/Identity.h - who a peer actually is, provable over an open network.
//
// THE PROBLEM. Until now a peer's identity was a display name it typed. On a trusted
// LAN that is fine. On the open internet it means anyone who can reach the port can
// claim to be anyone, take locks on entities they do not own, write to the project, and
// sign commits in someone else's name - and the journal would record that attribution
// permanently, because a commit's author is durable.
//
// THE MODEL: SSH's, not a password's.
//
//   * Every install generates an ECDSA P-256 KEYPAIR once. The private key never leaves
//     the machine and is never transmitted.
//   * THE PUBLIC KEY IS THE IDENTITY. A peer is not "ben", it is a specific key; "ben"
//     is a label attached to it for the UI. Two people cannot collide, and nobody can
//     rename themselves into someone else's locks or history.
//   * On connect the host sends a random CHALLENGE; the peer signs it. A signature over
//     a fresh nonce proves possession of the private key without revealing it, and
//     cannot be replayed against a later session.
//   * The host keeps an ALLOWLIST of public keys. Joining a project is the host adding
//     a fingerprint, exactly like authorized_keys.
//
// Why not passwords: they need somewhere to be stored and something to verify them,
// which means either a server the author has to run and secure, or a secret in the
// client binary - and a secret in a binary that ships to other people is not a secret.
// Why not "no auth, it's a small team": the whole point of this change is that the
// session is now reachable from the entire internet.
//
// P-256 rather than Ed25519 because BCrypt (in-box on every supported Windows) exposes
// ECDSA P-256 universally, and adding a crypto dependency to ship one signature would
// be a worse trade than the curve choice.
//
// WHAT THIS DOES NOT DO: it authenticates the PEER, not the CHANNEL. Signed messages
// still cross the wire in the clear, so anyone on the path can read a scene delta and
// tamper with an unsigned one. Confidentiality and integrity of the stream need TLS -
// see the transport note in CollabTypes.h. Identity is the half that must be right
// first, because without it TLS just gives you a private conversation with a stranger
// wearing your colleague's name.
#pragma once

#include "Core/Types.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace hbe::collab {

// An uncompressed P-256 public key (X and Y, 32 bytes each). This IS a peer's identity.
using PublicKey = std::array<u8, 64>;
using Signature = std::array<u8, 64>; // r || s

// A short, human-comparable form of a public key, for the "is this really Ben?" check
// people actually do out loud. SHA-256 of the key, first 8 bytes, grouped hex.
std::string Fingerprint(const PublicKey& k);

// A stable 64-bit id DERIVED from the public key, for use where a compact key is needed
// (map keys, the wire's UserId). Derived, never assigned: a counter would collide
// across hosts, and a durable journal recording author=3 would be wrong forever.
u64 PeerIdFromKey(const PublicKey& k);

// One install's keypair. The private half is held opaquely by the OS key store handle
// and is deliberately not exposed as bytes anywhere in this API.
class Identity {
public:
    Identity() = default;
    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;

    // Loads the keypair at `file`, generating and saving one if absent. The file holds
    // the PRIVATE key, so it is written with a temp-then-rename and should live in the
    // per-user profile - never inside a project, and never inside anything the updater
    // replaces.
    bool LoadOrCreate(const std::filesystem::path& file);
    bool Valid() const { return key_ != nullptr; }

    const PublicKey& Public() const { return pub_; }
    u64 PeerId() const { return PeerIdFromKey(pub_); }

    // Signs arbitrary bytes (internally over their SHA-256). False if unusable.
    bool Sign(const u8* data, usize n, Signature& out) const;

private:
    void* key_ = nullptr; // BCRYPT_KEY_HANDLE, opaque so this header stays Windows-free
    PublicKey pub_{};
};

// Verifies a signature against a public key. Static and stateless: the verifier does
// not need a keypair of its own, which is what lets a headless server check a peer.
bool Verify(const PublicKey& k, const u8* data, usize n, const Signature& sig);

// A fresh, unguessable challenge. 32 bytes from the OS CSPRNG - not a counter and not
// a timestamp, either of which a listener could predict and pre-sign.
using Challenge = std::array<u8, 32>;
bool MakeChallenge(Challenge& out);

// Who is allowed in. The host's authorized_keys.
//
// DEFAULT-DENY. An empty list admits NOBODY, deliberately: the failure mode of
// default-allow is a project that is wide open the moment it is exposed, and the person
// running it has no way to notice.
class Allowlist {
public:
    bool Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    // BY VALUE, deliberately. The natural call is Add(list.Entries()[i].key, ...) or
    // Add(knock.key, ...) - a reference INTO a container this call then mutates. A
    // const& parameter would dangle mid-function the moment push_back reallocated or an
    // erase shifted the element out from under it, and the symptom would be a rare
    // corrupted key rather than a crash. 64 bytes is not worth the hazard.
    void Add(PublicKey k, const std::string& label);
    bool Remove(PublicKey k);
    bool Allows(const PublicKey& k) const;
    // The label the HOST assigned. Never the name the peer sent: a display name from
    // the wire is attacker-controlled and must not be what a lock badge shows.
    std::string LabelFor(const PublicKey& k) const;

    struct Entry {
        PublicKey key{};
        std::string label;
    };
    const std::vector<Entry>& Entries() const { return entries_; }
    bool Empty() const { return entries_.empty(); }

private:
    std::vector<Entry> entries_;
};

bool IdentitySelfTest(); // --test-identity

} // namespace hbe::collab
