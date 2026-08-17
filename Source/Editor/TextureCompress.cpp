// Editor/TextureCompress.cpp - BC encoding via stb_dxt (see TextureCompress.h).
#include "Editor/TextureCompress.h"

#include "Core/Log.h"
#include "RHI/RHI.h"

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <algorithm>
#include <cstring>

namespace hbe::tex {
namespace {

// Gather the 4x4 RGBA block whose top-left is (bx*4, by*4), clamping to the mip edges so a
// width/height that is not a multiple of 4 pads by replicating the border texel (standard).
void GatherBlockRGBA(const u8* mip, u32 w, u32 h, u32 bx, u32 by, u8 out[64]) {
    for (u32 ry = 0; ry < 4; ++ry) {
        const u32 sy = std::min(by * 4 + ry, h - 1);
        for (u32 rx = 0; rx < 4; ++rx) {
            const u32 sx = std::min(bx * 4 + rx, w - 1);
            const u8* t = mip + (static_cast<usize>(sy) * w + sx) * 4;
            u8* d = out + (ry * 4 + rx) * 4;
            d[0] = t[0]; d[1] = t[1]; d[2] = t[2]; d[3] = t[3];
        }
    }
}

} // namespace

std::optional<uaf::Texture> CompressToBC(const uaf::Texture& src, BCKind kind, bool srgb) {
    if (kind == BCKind::None) return std::nullopt; // caller keeps the uncompressed texture
    if (src.pixels.empty()) return std::nullopt;
    const rhi::Format sf = static_cast<rhi::Format>(src.format);
    if (sf != rhi::Format::R8G8B8A8_UNORM && sf != rhi::Format::R8G8B8A8_SRGB) return std::nullopt;
    // D3D12 requires the MIP-0 dimensions of a block-compressed resource to be multiples of 4
    // (sub-4 tail mips are fine; Vulkan is more lenient, but we target both). Padding would shift
    // the UV mapping, so a non-mult-4 texture simply stays uncompressed (a rare minority - artists
    // author power-of-2 / mult-4). The caller keeps the original .uaf when this returns nullopt.
    if ((src.width & 3u) != 0u || (src.height & 3u) != 0u) return std::nullopt;

    rhi::Format outFmt = rhi::Format::BC3_UNORM;
    u32 blockBytes = 16;
    switch (kind) {
        case BCKind::ColorRGBA:     outFmt = srgb ? rhi::Format::BC3_SRGB : rhi::Format::BC3_UNORM; blockBytes = 16; break;
        case BCKind::ColorRGB:      outFmt = srgb ? rhi::Format::BC1_SRGB : rhi::Format::BC1_UNORM; blockBytes = 8;  break;
        case BCKind::NormalRG:      outFmt = rhi::Format::BC5_UNORM; blockBytes = 16; break;
        case BCKind::SingleChannel: outFmt = rhi::Format::BC4_UNORM; blockBytes = 8;  break;
    }

    uaf::Texture out;
    out.width = src.width;
    out.height = src.height;
    out.mipCount = src.mipCount < 1 ? 1 : src.mipCount;
    out.format = static_cast<u32>(outFmt);
    // Reserve the exact block-packed size across all mips.
    u64 totalBlocks = 0;
    for (u32 mip = 0; mip < out.mipCount; ++mip) {
        const u32 mw = std::max(1u, src.width >> mip), mh = std::max(1u, src.height >> mip);
        totalBlocks += static_cast<u64>((mw + 3) / 4) * ((mh + 3) / 4);
    }
    out.pixels.reserve(static_cast<usize>(totalBlocks * blockBytes));

    usize srcOff = 0;
    for (u32 mip = 0; mip < out.mipCount; ++mip) {
        const u32 mw = std::max(1u, src.width >> mip), mh = std::max(1u, src.height >> mip);
        const usize mipBytes = static_cast<usize>(mw) * mh * 4u;
        if (srcOff + mipBytes > src.pixels.size()) {
            HBE_ERROR("tex: source mip {} runs past the RGBA8 payload ({} > {}); aborting BC encode.",
                      mip, srcOff + mipBytes, src.pixels.size());
            return std::nullopt;
        }
        const u8* mipSrc = src.pixels.data() + srcOff;
        const u32 bxN = (mw + 3) / 4, byN = (mh + 3) / 4;
        for (u32 by = 0; by < byN; ++by) {
            for (u32 bx = 0; bx < bxN; ++bx) {
                u8 block[64];
                GatherBlockRGBA(mipSrc, mw, mh, bx, by, block);
                u8 dst[16];
                switch (kind) {
                    case BCKind::ColorRGBA:
                        stb_compress_dxt_block(dst, block, 1 /*alpha -> BC3*/, STB_DXT_HIGHQUAL);
                        break;
                    case BCKind::ColorRGB:
                        stb_compress_dxt_block(dst, block, 0 /*BC1*/, STB_DXT_HIGHQUAL);
                        break;
                    case BCKind::NormalRG: {
                        u8 rg[32];
                        for (int i = 0; i < 16; ++i) { rg[i * 2] = block[i * 4]; rg[i * 2 + 1] = block[i * 4 + 1]; }
                        stb_compress_bc5_block(dst, rg);
                        break;
                    }
                    case BCKind::SingleChannel: {
                        u8 r[16];
                        for (int i = 0; i < 16; ++i) r[i] = block[i * 4];
                        stb_compress_bc4_block(dst, r);
                        break;
                    }
                }
                out.pixels.insert(out.pixels.end(), dst, dst + blockBytes);
            }
        }
        srcOff += mipBytes;
    }
    return out;
}

bool CompressSelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "bc-encode FAIL: %s\n", what); ++fails; }
    };

    // A 12x8 RGBA8 image (mult-of-4, so BC is allowed) with 2 mips (12x8, 6x4) - the 6x4 tail
    // exercises a mip whose block grid (2x1) is not a clean halving.
    const u32 W = 12, H = 8;
    uaf::Texture src;
    src.width = W; src.height = H; src.mipCount = 2;
    src.format = static_cast<u32>(rhi::Format::R8G8B8A8_UNORM);
    const auto push = [&](u32 w, u32 h) {
        for (u32 i = 0; i < w * h; ++i) {
            src.pixels.push_back(static_cast<u8>(i * 7));
            src.pixels.push_back(static_cast<u8>(i * 3 + 40));
            src.pixels.push_back(static_cast<u8>(i * 5 + 90));
            src.pixels.push_back(255);
        }
    };
    push(12, 8);
    push(6, 4);

    for (const BCKind kind : {BCKind::ColorRGBA, BCKind::ColorRGB, BCKind::NormalRG, BCKind::SingleChannel}) {
        const std::optional<uaf::Texture> bc = CompressToBC(src, kind, /*srgb*/ false);
        check(bc.has_value(), "CompressToBC must succeed on an RGBA8 input");
        if (!bc) continue;
        // Expected block-packed size must match exactly what the RHI staging will read.
        const rhi::Format f = static_cast<rhi::Format>(bc->format);
        check(rhi::IsBlockCompressed(f), "output format must be block-compressed");
        u64 expect = 0;
        for (u32 mip = 0; mip < bc->mipCount; ++mip) {
            const u32 mw = std::max(1u, W >> mip), mh = std::max(1u, H >> mip);
            expect += rhi::MipByteSize(f, mw, mh, 0);
        }
        check(bc->pixels.size() == expect, "BC payload size must equal the sum of block-aware mip sizes");
        check(bc->width == W && bc->height == H && bc->mipCount == 2, "dims/mipCount preserved");
    }

    // A non-RGBA8 input must be refused (caller keeps the uncompressed texture).
    uaf::Texture hdr = src;
    hdr.format = static_cast<u32>(rhi::Format::R16G16B16A16_FLOAT);
    check(!CompressToBC(hdr, BCKind::ColorRGBA, false).has_value(), "HDR input must be refused");

    // A non-multiple-of-4 texture must be refused (D3D12 rejects such BC resources).
    uaf::Texture odd;
    odd.width = 10; odd.height = 6; odd.mipCount = 1;
    odd.format = static_cast<u32>(rhi::Format::R8G8B8A8_UNORM);
    odd.pixels.assign(static_cast<usize>(10) * 6 * 4, 128);
    check(!CompressToBC(odd, BCKind::ColorRGBA, false).has_value(),
          "a non-mult-4 texture must be refused (kept uncompressed)");

    if (fails == 0)
        std::printf("bc-encode: BC3/BC1/BC5/BC4 encode a non-multiple-of-4 mipped image to the "
                    "exact block-packed size the RHI staging expects; non-RGBA8 is refused\n");
    return fails == 0;
}

} // namespace hbe::tex
