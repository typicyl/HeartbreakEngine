// Assets/Compression.cpp - see Compression.h.
#include "Assets/Compression.h"

#include "Core/Log.h"

#include <cstring>

#include <zstd.h>
#include <zlib.h> // assimp's zlibstatic (already on the include path); portable fallback

// Legacy LZMS is READ-ONLY back-compat for pre-v5 packs and exists only on Windows.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <compressapi.h> // Cabinet.lib
#endif

namespace hbe::comp {
namespace {

// zstd's default cook level. 19 is near the top of the "normal" range: ratio close to
// the old LZMS, still deterministic and reasonable to run offline, and decode speed is
// independent of it. (22 is the ultra range; the marginal ratio is not worth the cook
// time here.)
constexpr int kZstdCookLevel = 19;

std::optional<std::vector<u8>> ZstdCompress(const u8* data, usize size, int level) {
    const usize bound = ZSTD_compressBound(size);
    std::vector<u8> out(bound);
    const usize n =
        ZSTD_compress(out.data(), bound, data, size, level > 0 ? level : kZstdCookLevel);
    if (ZSTD_isError(n)) return std::nullopt;
    out.resize(n);
    return out;
}

std::optional<std::vector<u8>> ZstdDecompress(const u8* data, usize size, usize rawSize) {
    std::vector<u8> out(rawSize);
    const usize n = ZSTD_decompress(out.data(), rawSize, data, size);
    if (ZSTD_isError(n) || n != rawSize) return std::nullopt;
    return out;
}

std::optional<std::vector<u8>> ZlibCompress(const u8* data, usize size, int level) {
    uLongf bound = ::compressBound(static_cast<uLong>(size));
    std::vector<u8> out(bound);
    const int lvl = level > 0 ? level : Z_BEST_COMPRESSION;
    const int rc = ::compress2(out.data(), &bound, data, static_cast<uLong>(size), lvl);
    if (rc != Z_OK) return std::nullopt;
    out.resize(bound);
    return out;
}

std::optional<std::vector<u8>> ZlibDecompress(const u8* data, usize size, usize rawSize) {
    std::vector<u8> out(rawSize);
    uLongf got = static_cast<uLongf>(rawSize);
    const int rc = ::uncompress(out.data(), &got, data, static_cast<uLong>(size));
    if (rc != Z_OK || got != rawSize) return std::nullopt;
    return out;
}

#ifdef _WIN32
// Legacy Windows LZMS decode only (pre-v5 packs). Never encoded from here.
std::optional<std::vector<u8>> LzmsDecompress(const u8* data, usize size, usize rawSize) {
    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (!::CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decomp)) return std::nullopt;
    std::vector<u8> out(rawSize);
    SIZE_T written = 0;
    const bool ok = ::Decompress(decomp, const_cast<u8*>(data), size, out.data(), out.size(),
                                 &written);
    ::CloseDecompressor(decomp);
    if (!ok || written != rawSize) return std::nullopt;
    return out;
}
#endif

} // namespace

const char* ToString(Codec c) {
    switch (c) {
        case Codec::None:    return "None";
        case Codec::Zstd:    return "Zstd";
        case Codec::Zlib:    return "Zlib";
        case Codec::LzmsWin: return "LZMS(win)";
    }
    return "?";
}

bool CanDecode(Codec c) {
    switch (c) {
        case Codec::None:
        case Codec::Zstd:
        case Codec::Zlib:
            return true;
        case Codec::LzmsWin:
#ifdef _WIN32
            return true;
#else
            return false;
#endif
    }
    return false;
}

bool CanEncode(Codec c) {
    return c == Codec::None || c == Codec::Zstd || c == Codec::Zlib;
}

int DefaultZstdLevel() { return kZstdCookLevel; }

std::optional<std::vector<u8>> Compress(Codec codec, const u8* data, usize size, int level) {
    switch (codec) {
        case Codec::None:    return std::vector<u8>(data, data + size);
        case Codec::Zstd:    return ZstdCompress(data, size, level);
        case Codec::Zlib:    return ZlibCompress(data, size, level);
        case Codec::LzmsWin: return std::nullopt; // never encode legacy LZMS
    }
    return std::nullopt;
}

std::optional<std::vector<u8>> Decompress(Codec codec, const u8* data, usize size, usize rawSize) {
    switch (codec) {
        case Codec::None:
            if (size != rawSize) return std::nullopt;
            return std::vector<u8>(data, data + size);
        case Codec::Zstd: return ZstdDecompress(data, size, rawSize);
        case Codec::Zlib: return ZlibDecompress(data, size, rawSize);
        case Codec::LzmsWin:
#ifdef _WIN32
            return LzmsDecompress(data, size, rawSize);
#else
            HBE_ERROR("comp: this pack uses legacy Windows LZMS and cannot be read on this "
                      "platform. Re-cook the packs (they will use portable zstd).");
            return std::nullopt;
#endif
    }
    return std::nullopt;
}

// Murmur3-fmix64-finalized block mix. Reads 8 bytes/iteration little-endian; the format
// is declared little-endian, so this is deterministic across all shipping targets.
u64 Hash64(const u8* data, usize size, u64 seed) {
    constexpr u64 c1 = 0xff51afd7ed558ccdULL;
    constexpr u64 c2 = 0xc4ceb9fe1a85ec53ULL;
    constexpr u64 gold = 0x9E3779B97F4A7C15ULL;
    const auto rotl = [](u64 x, int r) { return (x << r) | (x >> (64 - r)); };

    u64 h = seed ^ gold ^ (static_cast<u64>(size) * c1);
    usize i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 k = 0;
        std::memcpy(&k, data + i, 8);
        k *= c1;
        k ^= k >> 33;
        k *= c2;
        h ^= k;
        h = rotl(h, 27) * 0x100000001b3ULL + gold;
    }
    u64 tail = 0;
    for (usize j = 0; i < size; ++i, ++j) tail |= static_cast<u64>(data[i]) << (8 * j);
    tail *= c1;
    tail ^= tail >> 33;
    h ^= tail;
    // fmix64
    h ^= h >> 33;
    h *= c1;
    h ^= h >> 33;
    h *= c2;
    h ^= h >> 33;
    return h;
}

// ---------------------------------------------------------------------------
// Self-test (--test-compress)
// ---------------------------------------------------------------------------
bool SelfTest() {
    u32 fails = 0;
    const auto fail = [&](const char* what) {
        HBE_ERROR("comp: SelfTest FAIL - {}", what);
        ++fails;
    };

    // A corpus spanning the edge cases a cook actually hits: empty, sub-8 (hash tail
    // path), highly compressible (runs), and effectively incompressible (LCG noise).
    std::vector<std::vector<u8>> corpus;
    corpus.push_back({});                                  // empty
    corpus.push_back({0x42});                              // 1 byte (hash tail)
    corpus.push_back(std::vector<u8>(7, 0xAB));            // 7 bytes (hash tail, no block)
    corpus.push_back(std::vector<u8>(64 * 1024, 0x00));    // very compressible
    {
        std::vector<u8> noise(200000);
        u32 s = 0x1234567u;
        for (u8& b : noise) { s = s * 1664525u + 1013904223u; b = static_cast<u8>(s >> 24); }
        corpus.push_back(std::move(noise));                // ~incompressible
    }

    const Codec codecs[] = {Codec::None, Codec::Zstd, Codec::Zlib};
    for (const std::vector<u8>& in : corpus) {
        for (Codec c : codecs) {
            if (!CanEncode(c)) continue;
            auto packed = Compress(c, in.data(), in.size(), 0);
            if (!packed) { fail("Compress returned nullopt"); continue; }
            auto back = Decompress(c, packed->data(), packed->size(), in.size());
            if (!back) { fail("Decompress returned nullopt"); continue; }
            if (*back != in) fail("round-trip mismatch");
        }
    }

    // Hash: deterministic, and a single-bit change flips it (discriminating).
    {
        std::vector<u8> a(1000);
        for (usize i = 0; i < a.size(); ++i) a[i] = static_cast<u8>(i * 7 + 1);
        const u64 h1 = Hash64(a.data(), a.size());
        const u64 h2 = Hash64(a.data(), a.size());
        if (h1 != h2) fail("hash not deterministic");
        std::vector<u8> b = a;
        b[500] ^= 0x01;
        if (Hash64(b.data(), b.size()) == h1) fail("hash did not change on a 1-bit edit");
        if (Hash64(nullptr, 0) == Hash64(a.data(), a.size())) fail("empty vs non-empty collide");
    }

    // A decode with the wrong rawSize must fail rather than silently truncate.
    {
        const std::vector<u8> in(4096, 0x5A);
        auto packed = Compress(Codec::Zstd, in.data(), in.size(), 0);
        if (packed && Decompress(Codec::Zstd, packed->data(), packed->size(), in.size() + 1))
            fail("Zstd decode accepted a wrong rawSize");
    }

    if (fails == 0)
        HBE_INFO("comp: SelfTest passed (zstd+zlib round-trip, hash determinism, level {}).",
                 kZstdCookLevel);
    return fails == 0;
}

} // namespace hbe::comp
