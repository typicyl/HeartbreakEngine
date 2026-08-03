// Collab/Signaling.h - how two peers find each other, and why it needs no server.
//
// ICE cannot bootstrap itself. Before two machines can punch a hole to each other they
// have to exchange one description each: what addresses they think they have, and the
// fingerprint of the DTLS key they will use. That exchange is the one part of a
// peer-to-peer system that is not, itself, peer-to-peer.
//
// THE POINT OF THIS FILE IS TO MAKE THAT EXCHANGE UNTRUSTED AND TINY.
//
// It carries no secrets and grants no authority. Identity is proven INSIDE the tunnel by
// SecureChannel, after connecting - so whoever carries the blob (Discord, email, a text
// message, a sticky note) never has to be trusted, and cannot become a party to the
// session by tampering with what they carry. Swap the DTLS fingerprint in transit and the
// connection still forms; the peer then fails to prove it holds its key and is dropped.
//
// That is what lets the default implementation be NOTHING: two strings, copied and pasted
// by hand. No rendezvous service, no account, no infrastructure, nothing to keep running,
// and nothing that stops working when a domain lapses. A convenience rendezvous can be
// added later behind the same two functions without changing a line of the security
// story, because there is no security in this layer to change.
//
// NON-TRICKLE ICE, deliberately. We wait for candidate gathering to finish so the whole
// description fits in ONE blob per side. Trickle ICE connects a second or two faster and
// would need a live channel between the peers to stream candidates through - which is
// exactly the server we are refusing to require.
#pragma once

#include "Collab/Identity.h"

#include <string>

namespace hbe::collab {

// One side's half of the exchange: the host's invitation, or the guest's reply.
struct SessionBlob {
    bool isAnswer = false; // false = the host's invitation, true = the guest's reply
    // ADVISORY ONLY. Lets the host pre-authorise a colleague instead of having to watch
    // for a refusal first. Safe to act on because authorising this key authorises the
    // KEY: an impostor who pastes someone else's key into a blob still cannot produce
    // that key's signature, so it gains them nothing. Never treat it as proof of who
    // sent the blob.
    PublicKey claimedKey{};
    std::string sdp; // the WebRTC session description, candidates included
};

// Text safe to paste into a chat window: a short prefix so a human can tell what it is,
// then base64. Single line, no spaces.
std::string EncodeSessionBlob(const SessionBlob& b);
bool DecodeSessionBlob(const std::string& text, SessionBlob& out);

bool SignalingSelfTest(); // part of --test-webrtc

} // namespace hbe::collab
