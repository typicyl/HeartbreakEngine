// Navigation/NavTileCodec.h - the ONE DetourTileCache compressor + allocator the
// engine uses, shared verbatim by the editor baker and the runtime.
//
// WHY THIS FILE EXISTS. A DetourTileCache tile is a COMPRESSED heightfield layer.
// The editor produces those bytes (dtBuildTileCacheLayer) and the runtime consumes
// them (dtTileCache decompresses on buildNavMeshTile). If the two used different
// compressors the runtime would read garbage - so the compressor is defined ONCE,
// here, in a source that compiles into BOTH engine libraries (HBE_NAV_SOURCES), and
// both the baker (Source/Navigation/NavBaker.cpp, editor-only) and the runtime
// manager (Source/Navigation/NavMesh.cpp) construct THIS type. Bake==runtime by
// construction, not by convention.
//
// The compressor is zlib (deflate/inflate) on the zlibstatic the build already
// links - not fastlz (which the RecastDemo bundles) - so no new third-party file
// enters the tree. This is an INTERNAL nav header: it names Detour types, so it is
// included only by the two nav .cpp files above, never by an engine-facing API
// header (NavWorld.h stays Detour-free).
#pragma once

#include "DetourTileCacheBuilder.h" // dtTileCacheCompressor / dtTileCacheAlloc

namespace hbe {
namespace nav {

// zlib-backed DetourTileCache compressor. Stateless; cheap to construct per bake or
// per manager. maxCompressedSize uses zlib's exact compressBound so the caller's
// output buffer is always large enough (deflate can expand tiny/incompressible
// buffers, which a naive "size * 1.05" would under-allocate).
struct NavTileCompressor : public dtTileCacheCompressor {
    ~NavTileCompressor() override = default;
    int maxCompressedSize(const int bufferSize) override;
    dtStatus compress(const unsigned char* buffer, const int bufferSize,
                      unsigned char* compressed, const int maxCompressedSize,
                      int* compressedSize) override;
    dtStatus decompress(const unsigned char* compressed, const int compressedSize,
                        unsigned char* buffer, const int maxBufferSize,
                        int* bufferSize) override;
};

// A bump allocator for DetourTileCache's per-update scratch (contours / poly mesh
// while a tile is rebuilt). Recast recommends a linear allocator here because a
// tile rebuild allocates and frees a burst of small blocks; reset() reclaims them
// all at once between tiles. Falls back to a fresh buffer if a rebuild needs more
// than the reserve (never fails the rebuild). Runtime-only, but lives here beside
// the compressor so NavMesh.cpp holds no Detour scaffolding of its own.
class NavTileLinearAllocator : public dtTileCacheAlloc {
public:
    explicit NavTileLinearAllocator(size_t initialCap);
    ~NavTileLinearAllocator() override;
    void reset() override;
    void* alloc(size_t size) override;
    void free(void* ptr) override;

private:
    void Resize(size_t cap);
    unsigned char* buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t top_ = 0;
    size_t high_ = 0;
};

} // namespace nav
} // namespace hbe
