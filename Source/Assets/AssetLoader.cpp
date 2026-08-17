// Assets/AssetLoader.cpp
#include "Assets/AssetLoader.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace hbe::assets {

namespace {

// 256-entry sRGB -> linear decode table (encode is done per-write).
const f32* SrgbDecodeTable() {
    static f32 table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; ++i) {
            const f32 c = i / 255.0f;
            table[i] = c <= 0.04045f ? c / 12.92f
                                     : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        built = true;
    }
    return table;
}

u8 EncodeSrgb(f32 v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const f32 c = v <= 0.0031308f ? v * 12.92f
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(c * 255.0f + 0.5f);
}

} // namespace

void GenerateMips(uaf::Texture& tex) {
    const auto fmt = static_cast<rhi::Format>(tex.format);
    const bool srgb = fmt == rhi::Format::R8G8B8A8_SRGB || fmt == rhi::Format::B8G8R8A8_SRGB;
    const bool rgba8 = srgb || fmt == rhi::Format::R8G8B8A8_UNORM ||
                       fmt == rhi::Format::B8G8R8A8_UNORM;
    if (!rgba8 || tex.mipCount > 1 || (tex.width < 2 && tex.height < 2)) return;
    if (tex.pixels.size() < static_cast<usize>(tex.width) * tex.height * 4) return;

    u32 mips = 1;
    for (u32 w = tex.width, h = tex.height; w > 1 || h > 1;
         w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
        ++mips;
    }

    usize total = 0;
    for (u32 m = 0, w = tex.width, h = tex.height; m < mips; ++m) {
        total += static_cast<usize>(w) * h * 4;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    std::vector<u8> all;
    all.reserve(total);
    all.insert(all.end(), tex.pixels.begin(),
               tex.pixels.begin() + static_cast<usize>(tex.width) * tex.height * 4);

    const f32* decode = SrgbDecodeTable();
    usize prevOffset = 0;
    u32 pw = tex.width, ph = tex.height;
    for (u32 m = 1; m < mips; ++m) {
        const u32 w = std::max(1u, pw / 2);
        const u32 h = std::max(1u, ph / 2);
        const usize offset = all.size();
        all.resize(offset + static_cast<usize>(w) * h * 4);
        const u8* src = all.data() + prevOffset;
        u8* dst = all.data() + offset;
        for (u32 y = 0; y < h; ++y) {
            const u32 y0 = std::min(y * 2, ph - 1);
            const u32 y1 = std::min(y * 2 + 1, ph - 1);
            for (u32 x = 0; x < w; ++x) {
                const u32 x0 = std::min(x * 2, pw - 1);
                const u32 x1 = std::min(x * 2 + 1, pw - 1);
                const u8* p00 = src + (static_cast<usize>(y0) * pw + x0) * 4;
                const u8* p01 = src + (static_cast<usize>(y0) * pw + x1) * 4;
                const u8* p10 = src + (static_cast<usize>(y1) * pw + x0) * 4;
                const u8* p11 = src + (static_cast<usize>(y1) * pw + x1) * 4;
                u8* out = dst + (static_cast<usize>(y) * w + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    if (srgb && c < 3) {
                        // Average in linear space; alpha stays linear.
                        const f32 lin = (decode[p00[c]] + decode[p01[c]] +
                                         decode[p10[c]] + decode[p11[c]]) * 0.25f;
                        out[c] = EncodeSrgb(lin);
                    } else {
                        out[c] = static_cast<u8>(
                            (static_cast<u32>(p00[c]) + p01[c] + p10[c] + p11[c] + 2) / 4);
                    }
                }
            }
        }
        prevOffset = offset;
        pw = w;
        ph = h;
    }

    tex.pixels = std::move(all);
    tex.mipCount = mips;
}

namespace {
std::atomic<bool> g_bcAvailable{false};
} // namespace

std::string BcVariantName(const std::string& uafRef) {
    static const std::string kExt = ".uaf";
    if (uafRef.size() > kExt.size() &&
        uafRef.compare(uafRef.size() - kExt.size(), kExt.size(), kExt) == 0) {
        return uafRef.substr(0, uafRef.size() - kExt.size()) + ".bc.uaf";
    }
    return uafRef;
}
void SetBlockCompressionAvailable(bool v) { g_bcAvailable.store(v, std::memory_order_relaxed); }
bool BlockCompressionAvailable() { return g_bcAvailable.load(std::memory_order_relaxed); }

rhi::TextureHandle LoadTexture(Renderer& renderer, const std::filesystem::path& uaf) {
    std::optional<uaf::Texture> tex = uaf::ReadTexture(uaf);
    if (!tex || tex->pixels.empty()) {
        HBE_ERROR("AssetLoader: failed to read texture '{}'.", uaf.string());
        return {};
    }
    GenerateMips(*tex);
    rhi::TextureDesc desc;
    desc.width = tex->width;
    desc.height = tex->height;
    desc.format = static_cast<rhi::Format>(tex->format);
    desc.mipCount = tex->mipCount;
    desc.pixels = tex->pixels.data();
    desc.debugName = "uaf_texture";
    return renderer.UploadTexture(desc);
}

std::optional<Model> LoadMesh(const std::filesystem::path& uaf) {
    std::optional<Model> model = uaf::ReadMesh(uaf);
    if (!model || model->empty()) {
        HBE_ERROR("AssetLoader: failed to read mesh '{}'.", uaf.string());
        return std::nullopt;
    }
    return model;
}

} // namespace hbe::assets
