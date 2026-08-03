// Collab/Signaling.cpp
#include "Collab/Signaling.h"

#include <cstdio>
#include <cstring>

namespace hbe::collab {

namespace {

constexpr char kPrefixOffer[] = "HBE-INVITE-1:";
constexpr char kPrefixAnswer[] = "HBE-REPLY-1:";
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string B64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const u32 v = (static_cast<u8>(in[i]) << 16) | (static_cast<u8>(in[i + 1]) << 8) |
                      static_cast<u8>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        u32 v = static_cast<u32>(static_cast<u8>(in[i])) << 16;
        const bool two = (i + 1 < in.size());
        if (two) v |= static_cast<u32>(static_cast<u8>(in[i + 1])) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += two ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

int B64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool B64Decode(const std::string& in, std::string& out) {
    out.clear();
    u32 acc = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = B64Value(c);
        if (v < 0) return false; // refuse junk rather than decoding a prefix of it
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

void AppendHex(std::string& s, const PublicKey& k) {
    static const char* kHex = "0123456789abcdef";
    for (const u8 b : k) {
        s += kHex[b >> 4];
        s += kHex[b & 0xF];
    }
}

bool ParseHexKey(const std::string& s, PublicKey& out) {
    if (s.size() != out.size() * 2) return false;
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (usize i = 0; i < out.size(); ++i) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

} // namespace

std::string EncodeSessionBlob(const SessionBlob& b) {
    // Body: a couple of headers, a separator, then the SDP verbatim. The SDP is
    // multi-line and goes LAST so nothing has to escape it.
    std::string body = "k=";
    AppendHex(body, b.claimedKey);
    body += "\n--\n";
    body += b.sdp;
    return std::string(b.isAnswer ? kPrefixAnswer : kPrefixOffer) + B64Encode(body);
}

bool DecodeSessionBlob(const std::string& text, SessionBlob& out) {
    // People paste with stray whitespace and newlines from chat clients; trim rather
    // than fail on something a human would call identical.
    usize a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\n' || text[a] == '\r' || text[a] == '\t'))
        ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\n' || text[b - 1] == '\r' ||
                     text[b - 1] == '\t'))
        --b;
    const std::string s = text.substr(a, b - a);

    const char* prefix = nullptr;
    if (s.rfind(kPrefixOffer, 0) == 0) {
        out.isAnswer = false;
        prefix = kPrefixOffer;
    } else if (s.rfind(kPrefixAnswer, 0) == 0) {
        out.isAnswer = true;
        prefix = kPrefixAnswer;
    } else {
        return false;
    }

    std::string body;
    if (!B64Decode(s.substr(std::strlen(prefix)), body)) return false;
    if (body.rfind("k=", 0) != 0) return false;
    const usize nl = body.find('\n');
    if (nl == std::string::npos) return false;
    if (!ParseHexKey(body.substr(2, nl - 2), out.claimedKey)) return false;
    const usize sep = body.find("\n--\n");
    if (sep == std::string::npos) return false;
    out.sdp = body.substr(sep + 4);
    return !out.sdp.empty();
}

bool SignalingSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("signaling FAIL: %s\n", what);
    };

    SessionBlob in;
    in.isAnswer = false;
    for (usize i = 0; i < in.claimedKey.size(); ++i) in.claimedKey[i] = static_cast<u8>(i * 7);
    // A realistic SDP: multi-line, with '=' and ':' throughout, which is exactly what a
    // naive key=value parser mangles.
    in.sdp = "v=0\r\no=- 42 0 IN IP4 127.0.0.1\r\ns=-\r\n"
             "a=fingerprint:sha-256 AB:CD:EF\r\na=candidate:1 1 UDP 123 10.0.0.1 5000 typ host\r\n";

    const std::string text = EncodeSessionBlob(in);
    check(text.rfind("HBE-INVITE-1:", 0) == 0, "an invitation should be recognisable at a glance");
    check(text.find('\n') == std::string::npos,
          "the blob must be ONE line - a chat client will reflow anything else");

    SessionBlob back;
    check(DecodeSessionBlob(text, back), "the blob should decode");
    check(back.sdp == in.sdp, "the SDP must survive EXACTLY - a mangled candidate list "
                              "fails to connect with no useful error");
    check(back.claimedKey == in.claimedKey, "the advertised key must survive");
    check(!back.isAnswer, "an invitation must not decode as a reply");

    // Pasted out of a chat window, with the whitespace people actually get.
    SessionBlob trimmed;
    check(DecodeSessionBlob("  \r\n" + text + "\n\n", trimmed) && trimmed.sdp == in.sdp,
          "surrounding whitespace from a paste must not break it");

    SessionBlob ans = in;
    ans.isAnswer = true;
    SessionBlob ansBack;
    check(DecodeSessionBlob(EncodeSessionBlob(ans), ansBack) && ansBack.isAnswer,
          "a reply must decode as a reply");

    // Refusals.
    SessionBlob junk;
    check(!DecodeSessionBlob("hello", junk), "arbitrary text must not decode");
    check(!DecodeSessionBlob("", junk), "empty text must not decode");
    check(!DecodeSessionBlob("HBE-INVITE-1:!!!!", junk),
          "a corrupted body must be REFUSED, not half-decoded into a broken SDP");
    check(!DecodeSessionBlob(text.substr(0, text.size() - 20), junk) ||
              junk.sdp != in.sdp,
          "a truncated blob must not yield the original SDP");

    if (fails == 0)
        std::printf("signaling: an invitation round-trips as one pasteable line with the "
                    "SDP byte-exact; whitespace is tolerated; junk, empty and corrupted "
                    "text are refused\n");
    return fails == 0;
}

} // namespace hbe::collab
