// Editor/TextureCompress.h - offline BC (block-compressed) texture encoding, at import.
//
// Takes an already-mipped RGBA8 `uaf::Texture` and returns a BC variant (BC3 color / BC5
// normals / BC4 single-channel / BC1 low-color) via stb_dxt. NON-DESTRUCTIVE: the caller
// writes the result to a SEPARATE `.uaf` and never touches the uncompressed source. The
// runtime never decodes on the CPU - it hands the BC bytes straight to the GPU, which samples
// the block format natively. Editor-only (the encoder is not linked into the shipped runtime).
#pragma once

#include "Assets/UAF.h"

#include <optional>

namespace hbe::tex {

// What the texture is FOR - picks the codec + channel packing. BC3 (DXT5) is the color default:
// good quality, 8bpp, and native to the PS3/RSX-class hardware this engine targets. BC7 would be
// marginally better but has no CPU encoder here (and PS3 can't sample it).
enum class BCKind {
    None,          // do NOT compress (keep RGBA8) - data maps whose channels are independent
                   // (metallic-roughness) band badly under BC's shared endpoint line, and a
                   // role-unknown standalone import can't be safely categorized.
    ColorRGBA,     // -> BC3 (DXT5)  - full correlated color + alpha, the default for color
    ColorRGB,      // -> BC1 (DXT1)  - 4bpp, opt-in low-color only (visible chroma artifacts)
    NormalRG,      // -> BC5         - tangent-space normals (R,G); the shader reconstructs Z
    SingleChannel, // -> BC4         - one channel (R), e.g. a mask / height
};

// Compress an RGBA8 (R8G8B8A8_UNORM/SRGB), fully-mipped `src` to a BC `uaf::Texture`. `srgb`
// selects the _SRGB block format for the color kinds (ignored for normal/single). Returns
// nullopt if `src` is empty or not RGBA8 (the caller then keeps the uncompressed texture). The
// output carries the same width/height/mipCount with BC bytes tightly packed, all mips.
std::optional<uaf::Texture> CompressToBC(const uaf::Texture& src, BCKind kind, bool srgb);

bool CompressSelfTest(); // --test-bc-encode: block math + round-trip sanity, no GPU

} // namespace hbe::tex
