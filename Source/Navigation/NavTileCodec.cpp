// Navigation/NavTileCodec.cpp - see NavTileCodec.h.
#include "Navigation/NavTileCodec.h"

#include "DetourAlloc.h"
#include "DetourStatus.h"

#include <zlib.h>

#include <cstring>

namespace hbe {
namespace nav {

int NavTileCompressor::maxCompressedSize(const int bufferSize) {
    if (bufferSize <= 0) return 16;
    return static_cast<int>(::compressBound(static_cast<uLong>(bufferSize)));
}

dtStatus NavTileCompressor::compress(const unsigned char* buffer, const int bufferSize,
                                     unsigned char* compressed, const int maxCompressedSize,
                                     int* compressedSize) {
    if (compressedSize) *compressedSize = 0;
    if (bufferSize < 0 || maxCompressedSize <= 0) return DT_FAILURE;
    uLongf destLen = static_cast<uLongf>(maxCompressedSize);
    // Z_BEST_SPEED: nav layers are small and this runs per tile at bake time; the
    // ratio difference against BEST_COMPRESSION is negligible on this data.
    const int rc = ::compress2(compressed, &destLen, buffer,
                               static_cast<uLong>(bufferSize), Z_BEST_SPEED);
    if (rc != Z_OK) return DT_FAILURE;
    if (compressedSize) *compressedSize = static_cast<int>(destLen);
    return DT_SUCCESS;
}

dtStatus NavTileCompressor::decompress(const unsigned char* compressed, const int compressedSize,
                                       unsigned char* buffer, const int maxBufferSize,
                                       int* bufferSize) {
    if (bufferSize) *bufferSize = 0;
    if (compressedSize <= 0 || maxBufferSize <= 0) return DT_FAILURE;
    uLongf destLen = static_cast<uLongf>(maxBufferSize);
    const int rc = ::uncompress(buffer, &destLen, compressed, static_cast<uLong>(compressedSize));
    if (rc != Z_OK) return DT_FAILURE;
    if (bufferSize) *bufferSize = static_cast<int>(destLen);
    return DT_SUCCESS;
}

// --- NavTileLinearAllocator ------------------------------------------------

NavTileLinearAllocator::NavTileLinearAllocator(size_t initialCap) {
    Resize(initialCap);
}

NavTileLinearAllocator::~NavTileLinearAllocator() {
    dtFree(buffer_);
}

void NavTileLinearAllocator::Resize(size_t cap) {
    if (buffer_) dtFree(buffer_);
    buffer_ = static_cast<unsigned char*>(dtAlloc(cap, DT_ALLOC_PERM));
    capacity_ = buffer_ ? cap : 0;
    top_ = 0;
}

void NavTileLinearAllocator::reset() {
    if (top_ > high_) high_ = top_;
    top_ = 0;
}

void* NavTileLinearAllocator::alloc(size_t size) {
    // Round up to keep returned blocks 8-byte aligned (Detour writes structs here).
    size = (size + 7u) & ~static_cast<size_t>(7u);
    if (!buffer_ || top_ + size > capacity_) {
        // Grow to fit the burst: reallocating mid-reset would strand live blocks, so
        // only grow when empty; otherwise fall back to a heap block Detour will free.
        if (top_ == 0) {
            Resize(size > capacity_ * 2 ? size : capacity_ * 2);
            if (!buffer_ || size > capacity_) return dtAlloc(size, DT_ALLOC_TEMP);
        } else {
            return dtAlloc(size, DT_ALLOC_TEMP);
        }
    }
    unsigned char* mem = &buffer_[top_];
    top_ += size;
    return mem;
}

void NavTileLinearAllocator::free(void* ptr) {
    // Blocks inside the reserve are reclaimed wholesale by reset(); anything outside
    // it came from the dtAlloc fallback above and must be released individually.
    if (!ptr) return;
    unsigned char* p = static_cast<unsigned char*>(ptr);
    if (buffer_ && p >= buffer_ && p < buffer_ + capacity_) return; // owned by the arena
    dtFree(ptr);
}

} // namespace nav
} // namespace hbe
