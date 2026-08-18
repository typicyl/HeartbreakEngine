// Assets/Compression.h - the engine's ONE portable (de)compression + content-hash seam.
//
// WHY THIS EXISTS. The pack container used to compress every slot with LZMS through the
// Windows Compression API (compressapi.h / Cabinet.lib). That is a hard portability
// blocker: the SHIPPED runtime's pack reader would not even compile on a non-Windows
// target, and an LZMS-compressed pack is unreadable off Windows even if it did. The
// cooked-asset FORMAT must not depend on any OS API. This module routes all pack (and
// future per-asset) compression through a small, portable codec seam:
//
//   * Zstd  - the default cooked codec: high ratio, and MUCH faster to DECODE than LZMS
//             (decode speed is what a load path pays), pure C, zero transitive deps,
//             compiles on every target. This is what new (v5) packs use.
//   * Zlib  - already vendored (assimp's zlibstatic; the .hbnav codec uses it). A
//             portable fallback / alternative with a smaller code size than zstd.
//   * LzmsWin - READ-ONLY back-compat for packs cooked before v5. Available only on
//             Windows; a v5 build on any OS reads v5 (zstd) packs, and additionally
//             reads the legacy LZMS packs when it happens to be Windows.
//   * None  - stored verbatim.
//
// NO third-party type (ZSTD_*, z_stream, COMPRESSOR_HANDLE) crosses this header, so the
// runtime/editor libraries keep the "no external type in an engine header" discipline
// and a consumer never transitively includes <windows.h> or <zstd.h>.
#pragma once

#include "Core/Types.h"

#include <optional>
#include <vector>

namespace hbe::comp {

// The on-disk codec id. These values are a SHIPPING CONTRACT - they are written into
// the pack/asset header and must never be renumbered (append only). Deliberately NOT
// the Windows COMPRESS_ALGORITHM_* enum values, which are a platform constant that has
// no business in a portable format.
enum class Codec : u32 {
    None    = 0, // stored verbatim
    Zstd    = 1, // Zstandard (default for cooked data)
    Zlib    = 2, // raw zlib deflate (already-vendored fallback)
    LzmsWin = 3, // legacy Windows LZMS - READ-ONLY back-compat, Windows only
};

const char* ToString(Codec c);

// True when this build can DECODE the codec. Zstd/Zlib/None are always available;
// LzmsWin only on Windows. Encoding LzmsWin is never offered (it is legacy-read-only).
bool CanDecode(Codec c);
// True when this build can ENCODE the codec (LzmsWin is never encodable here).
bool CanEncode(Codec c);

// Compress `size` bytes. `level` is codec-specific; pass 0 for the module's default
// (a high, offline-cook-appropriate setting). Returns nullopt on failure or when the
// codec cannot be encoded by this build. `None` returns a verbatim copy.
std::optional<std::vector<u8>> Compress(Codec codec, const u8* data, usize size, int level = 0);

// Decompress `size` bytes to exactly `rawSize` bytes. Returns nullopt on failure, a
// size mismatch, or an unavailable codec. `rawSize` is authoritative (the caller stores
// it in the container), so the output buffer is sized once with no reallocation.
std::optional<std::vector<u8>> Decompress(Codec codec, const u8* data, usize size, usize rawSize);

// A portable, deterministic 64-bit content hash (Murmur-style finalized mix, reading 8
// bytes at a time little-endian). NOT cryptographic - it exists for (a) corruption
// detection on load and (b) content-dedup keying, where an exact byte compare confirms
// any hash match. Deterministic across LE targets, which is the format contract.
u64 Hash64(const u8* data, usize size, u64 seed = 0);

// Convenience default cook level for zstd (high ratio; offline). Exposed so tooling can
// report it. Decode cost is codec-fixed and low regardless of the encode level.
int DefaultZstdLevel();

// --test-compress: round-trips every encodable codec across empty/tiny/large + highly
// compressible and incompressible buffers, asserts byte-exact decode, and checks the
// content hash is deterministic and discriminating. Headless; no GPU/project. Returns
// false (logging each failure) on any mismatch.
bool SelfTest();

} // namespace hbe::comp
