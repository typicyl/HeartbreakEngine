// Collab/Identity.cpp - ECDSA P-256 identity via Windows CNG (in-box, no dependency).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "Collab/Identity.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#pragma comment(lib, "bcrypt.lib")

namespace hbe::collab {

namespace fs = std::filesystem;

namespace {

constexpr u32 kKeyFileMagic = 0x3159454Bu; // "KEY1"

bool Sha256(const u8* data, usize n, u8 out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = ::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) {
        ok = ::BCryptHashData(h, const_cast<PUCHAR>(data), static_cast<ULONG>(n), 0) == 0 &&
             ::BCryptFinishHash(h, out, 32, 0) == 0;
        ::BCryptDestroyHash(h);
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// A CNG ECC blob is {BCRYPT_ECCKEY_BLOB header, X, Y} and, for a private key, D.
struct EccBlob {
    std::vector<u8> bytes;
};

bool ExportPublic(BCRYPT_KEY_HANDLE key, PublicKey& out) {
    ULONG n = 0;
    if (::BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &n, 0) != 0)
        return false;
    std::vector<u8> blob(n);
    if (::BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), n, &n, 0) != 0)
        return false;
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return false;
    std::memcpy(out.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return true;
}

BCRYPT_KEY_HANDLE ImportPublic(const PublicKey& pub) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
        return nullptr;
    std::vector<u8> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pub.data(), 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    const bool ok = ::BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                          blob.data(), static_cast<ULONG>(blob.size()),
                                          0) == 0;
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? key : nullptr;
}

} // namespace

std::string Fingerprint(const PublicKey& k) {
    u8 d[32];
    if (!Sha256(k.data(), k.size(), d)) return "(unavailable)";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02x%02x:%02x%02x:%02x%02x:%02x%02x", d[0], d[1], d[2],
                  d[3], d[4], d[5], d[6], d[7]);
    return buf;
}

u64 PeerIdFromKey(const PublicKey& k) {
    u8 d[32];
    if (!Sha256(k.data(), k.size(), d)) return 0;
    u64 id = 0;
    std::memcpy(&id, d, sizeof(id));
    return id ? id : 1ull; // 0 means "nobody" everywhere else
}

Identity::~Identity() {
    if (key_) ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool Identity::LoadOrCreate(const fs::path& file) {
    if (key_) {
        ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = nullptr;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
        return false;

    // Load an existing private blob if we have one.
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (in) {
        const std::streamoff n = in.tellg();
        if (n > 4) {
            in.seekg(0);
            std::vector<u8> data(static_cast<usize>(n));
            in.read(reinterpret_cast<char*>(data.data()), n);
            u32 magic = 0;
            std::memcpy(&magic, data.data(), 4);
            if (magic == kKeyFileMagic) {
                BCRYPT_KEY_HANDLE k = nullptr;
                if (::BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &k,
                                          data.data() + 4,
                                          static_cast<ULONG>(data.size() - 4), 0) == 0) {
                    key_ = k;
                    if (ExportPublic(k, pub_)) {
                        ::BCryptCloseAlgorithmProvider(alg, 0);
                        return true;
                    }
                    ::BCryptDestroyKey(k);
                    key_ = nullptr;
                }
            }
        }
    }

    // Generate a fresh one.
    BCRYPT_KEY_HANDLE k = nullptr;
    if (::BCryptGenerateKeyPair(alg, &k, 256, 0) != 0 ||
        ::BCryptFinalizeKeyPair(k, 0) != 0) {
        if (k) ::BCryptDestroyKey(k);
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    ULONG n = 0;
    std::vector<u8> priv;
    if (::BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &n, 0) == 0) {
        priv.resize(n);
        if (::BCryptExportKey(k, nullptr, BCRYPT_ECCPRIVATE_BLOB, priv.data(), n, &n, 0) != 0)
            priv.clear();
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    if (priv.empty() || !ExportPublic(k, pub_)) {
        ::BCryptDestroyKey(k);
        return false;
    }
    key_ = k;

    // THIS FILE IS A PRIVATE KEY. Temp-then-rename so a crash cannot leave a truncated
    // one (which would silently become a NEW identity, losing every allowlist entry and
    // orphaning the peer's own signed history).
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(&kKeyFileMagic), 4);
        out.write(reinterpret_cast<const char*>(priv.data()),
                  static_cast<std::streamsize>(priv.size()));
        if (!out.good()) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool Identity::Sign(const u8* data, usize n, Signature& out) const {
    if (!key_) return false;
    u8 digest[32];
    if (!Sha256(data, n, digest)) return false;
    ULONG got = 0;
    if (::BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr, digest, 32,
                         out.data(), static_cast<ULONG>(out.size()), &got, 0) != 0)
        return false;
    return got == out.size();
}

bool Verify(const PublicKey& k, const u8* data, usize n, const Signature& sig) {
    BCRYPT_KEY_HANDLE key = ImportPublic(k);
    if (!key) return false;
    u8 digest[32];
    bool ok = Sha256(data, n, digest);
    if (ok) {
        ok = ::BCryptVerifySignature(key, nullptr, digest, 32,
                                     const_cast<PUCHAR>(sig.data()),
                                     static_cast<ULONG>(sig.size()), 0) == 0;
    }
    ::BCryptDestroyKey(key);
    return ok;
}

bool MakeChallenge(Challenge& out) {
    // The OS CSPRNG. A counter or a timestamp would be predictable, and a listener
    // could pre-compute a signature for a challenge it knows is coming.
    return ::BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// --- allowlist ---------------------------------------------------------------

void Allowlist::Add(PublicKey k, const std::string& label) {
    for (Entry& e : entries_)
        if (e.key == k) { e.label = label; return; }
    entries_.push_back(Entry{k, label});
}

bool Allowlist::Remove(PublicKey k) {
    for (usize i = 0; i < entries_.size(); ++i)
        if (entries_[i].key == k) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

bool Allowlist::Allows(const PublicKey& k) const {
    // DEFAULT-DENY, including the empty case. An empty allowlist admitting everyone is
    // the shape where a project is wide open the instant it is exposed and nobody
    // notices, because everything works.
    for (const Entry& e : entries_)
        if (e.key == k) return true;
    return false;
}

std::string Allowlist::LabelFor(const PublicKey& k) const {
    for (const Entry& e : entries_)
        if (e.key == k) return e.label;
    return {};
}

bool Allowlist::Load(const fs::path& file) {
    entries_.clear();
    std::ifstream in(file);
    if (!in) return false;
    // One entry per line: "<128 hex chars> <label>". Text on purpose - a host has to be
    // able to read, diff and hand-edit who can reach their project.
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 128) continue;
        PublicKey k{};
        bool ok = true;
        for (usize i = 0; i < 64 && ok; ++i) {
            const auto hex = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(line[i * 2]), lo = hex(line[i * 2 + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else k[i] = static_cast<u8>((hi << 4) | lo);
        }
        if (!ok) continue;
        std::string label = line.size() > 129 ? line.substr(129) : std::string();
        while (!label.empty() && (label.back() == '\r' || label.back() == ' ')) label.pop_back();
        entries_.push_back(Entry{k, label});
    }
    return true;
}

bool Allowlist::Save(const fs::path& file) const {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        static const char* kHex = "0123456789abcdef";
        for (const Entry& e : entries_) {
            for (const u8 b : e.key) {
                out << kHex[b >> 4] << kHex[b & 0xF];
            }
            out << ' ' << e.label << '\n';
        }
        if (!out.good()) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// --- self-test ---------------------------------------------------------------

bool IdentitySelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("identity FAIL: %s\n", what);
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_identity_test";
    fs::remove_all(dir, ec);

    Identity a;
    check(a.LoadOrCreate(dir / "a.hbkey"), "a keypair should generate");
    check(a.Valid(), "the generated identity should be valid");
    check(a.PeerId() != 0, "a peer id should derive from the key");

    // PERSISTENCE. Reloading must give the SAME identity - a new key on every launch
    // would drop the peer out of every allowlist and orphan its signed history.
    {
        Identity again;
        check(again.LoadOrCreate(dir / "a.hbkey"), "the keypair should reload");
        check(again.Public() == a.Public(), "reloading must give the SAME public key");
        check(again.PeerId() == a.PeerId(), "the peer id must be stable across loads");
    }

    Identity b;
    check(b.LoadOrCreate(dir / "b.hbkey"), "a second keypair should generate");
    check(b.Public() != a.Public(), "two installs must not share a key");
    check(b.PeerId() != a.PeerId(), "two installs must not share a peer id");
    check(Fingerprint(a.Public()) != Fingerprint(b.Public()),
          "two identities must have different fingerprints");

    // THE CHALLENGE-RESPONSE, which is the whole point.
    Challenge ch{};
    check(MakeChallenge(ch), "a challenge should generate");
    Challenge ch2{};
    MakeChallenge(ch2);
    check(ch != ch2, "two challenges must DIFFER, or a signature could be replayed");

    Signature sig{};
    check(a.Sign(ch.data(), ch.size(), sig), "signing a challenge should work");
    check(Verify(a.Public(), ch.data(), ch.size(), sig),
          "a genuine signature must verify against its own key");

    // ...and every way it must FAIL.
    check(!Verify(b.Public(), ch.data(), ch.size(), sig),
          "IMPERSONATION: a signature must NOT verify against a different peer's key");
    check(!Verify(a.Public(), ch2.data(), ch2.size(), sig),
          "REPLAY: a signature over one challenge must not verify against another");
    {
        Signature bad = sig;
        bad[0] ^= 0xFF;
        check(!Verify(a.Public(), ch.data(), ch.size(), bad),
              "a tampered signature must not verify");
    }
    {
        Challenge tampered = ch;
        tampered[7] ^= 0x01;
        check(!Verify(a.Public(), tampered.data(), tampered.size(), sig),
              "a signature must not verify over tampered data");
    }

    // THE ALLOWLIST, default-deny.
    {
        Allowlist list;
        check(list.Empty(), "a new allowlist is empty");
        check(!list.Allows(a.Public()),
              "AN EMPTY ALLOWLIST MUST ADMIT NOBODY - default-allow would leave a "
              "project wide open the moment it is exposed, with everything appearing "
              "to work");
        list.Add(a.Public(), "ana");
        check(list.Allows(a.Public()), "an added key should be allowed");
        check(!list.Allows(b.Public()), "an unlisted key must be refused");
        check(list.LabelFor(a.Public()) == "ana", "the host's label should be returned");

        const fs::path lf = dir / "authorized.txt";
        check(list.Save(lf), "the allowlist should save");
        Allowlist back;
        check(back.Load(lf), "the allowlist should load");
        check(back.Allows(a.Public()), "an allowed key must survive the round trip");
        check(!back.Allows(b.Public()), "a refused key must stay refused after a reload");
        check(back.LabelFor(a.Public()) == "ana", "labels must survive the round trip");

        check(list.Remove(a.Public()), "removal should report success");
        check(!list.Allows(a.Public()), "a removed key must be refused immediately");
    }

    fs::remove_all(dir, ec);
    if (fails == 0) {
        std::printf("identity: P-256 keypairs persist and stay stable; challenges never "
                    "repeat; a genuine signature verifies while impersonation, replay, "
                    "tampered signatures and tampered data all fail; an EMPTY allowlist "
                    "admits nobody\n");
    }
    return fails == 0;
}

} // namespace hbe::collab
