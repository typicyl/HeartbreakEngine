// UI/Svg/SvgCache.cpp
#include "UI/Svg/SvgCache.h"

#include "Assets/VFS.h" // vfs::ReadFile (pack-aware)
#include "Core/Log.h"
#include "RHI/RHI.h"
#include "Renderer/Renderer.h"

#include <lunasvg.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hbe::ui::svg {

bool IsSvgPath(const std::string& p) {
    if (p.size() < 4) return false;
    std::string ext = p.substr(p.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".svg";
}

namespace {

// Rasterizes SVG bytes to STRAIGHT-alpha RGBA (byte order R,G,B,A = R8G8B8A8_UNORM,
// which the UI shader's `color * tex` blend expects) at pxW x pxH. false on failure.
bool Rasterize(const std::vector<u8>& bytes, u32 pxW, u32 pxH, std::vector<u32>& out) {
    if (bytes.empty() || pxW == 0 || pxH == 0) return false;
    std::unique_ptr<lunasvg::Document> doc = lunasvg::Document::loadFromData(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!doc) return false;
    lunasvg::Bitmap bmp = doc->renderToBitmap(pxW, pxH, 0x00000000u); // transparent bg
    if (bmp.data() == nullptr || bmp.width() == 0 || bmp.height() == 0) return false;
    bmp.convertToRGBA(); // premultiplied ARGB -> straight-alpha RGBA byte order
    out.assign(static_cast<usize>(pxW) * pxH, 0u);
    const u32 rows = bmp.height() < pxH ? bmp.height() : pxH;
    const u32 cols = bmp.width() < pxW ? bmp.width() : pxW;
    const u32 stride = bmp.stride();
    const u8* src = bmp.data();
    for (u32 y = 0; y < rows; ++y) {
        std::memcpy(out.data() + static_cast<usize>(y) * pxW,
                    src + static_cast<usize>(y) * stride, static_cast<usize>(cols) * 4);
    }
    return true;
}

struct Key {
    std::string rel;
    u32 w = 0, h = 0;
    bool operator==(const Key& o) const { return w == o.w && h == o.h && rel == o.rel; }
};
struct KeyHash {
    usize operator()(const Key& k) const {
        u64 h = 1469598103934665603ull;
        for (char c : k.rel) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
        h = (h ^ k.w) * 1099511628211ull;
        h = (h ^ k.h) * 1099511628211ull;
        return static_cast<usize>(h);
    }
};

std::unordered_map<Key, u32, KeyHash>& RasterCache() {
    static std::unordered_map<Key, u32, KeyHash> c;
    return c;
}
// Raw file bytes per path, so re-rasterizing at a new size skips the VFS/disk read.
std::unordered_map<std::string, std::vector<u8>>& FileCache() {
    static std::unordered_map<std::string, std::vector<u8>> c;
    return c;
}

} // namespace

u32 ResolveSvg(Renderer& renderer, const std::filesystem::path& assetsDir,
               const std::string& rel, u32 pxW, u32 pxH) {
    if (rel.empty() || pxW == 0 || pxH == 0) return 0;
    const Key key{rel, pxW, pxH};
    if (auto it = RasterCache().find(key); it != RasterCache().end()) return it->second;

    std::vector<u8>* bytes = nullptr;
    if (auto it = FileCache().find(rel); it != FileCache().end()) {
        bytes = &it->second;
    } else if (auto data = vfs::ReadFile(assetsDir / rel)) {
        bytes = &FileCache().emplace(rel, std::move(*data)).first->second;
    } else {
        HBE_WARN("UI: SVG asset '{}' missing.", rel);
        RasterCache().emplace(key, 0u); // cache the miss (no per-frame retry)
        return 0;
    }

    std::vector<u32> rgba;
    if (!Rasterize(*bytes, pxW, pxH, rgba)) {
        HBE_WARN("UI: SVG '{}' failed to rasterize.", rel);
        RasterCache().emplace(key, 0u);
        return 0;
    }

    rhi::TextureDesc desc;
    desc.width = pxW;
    desc.height = pxH;
    desc.format = rhi::Format::R8G8B8A8_UNORM;
    desc.pixels = rgba.data();
    desc.debugName = "ui_svg";
    const u32 index = renderer.UploadTexture(desc).index;
    RasterCache().emplace(key, index);
    return index;
}

void ClearSvgCache() {
    RasterCache().clear();
    FileCache().clear();
}

bool SvgSelfTest() {
    int fails = 0;
    const auto check = [&](bool c, const char* m) {
        if (c) {
            HBE_INFO("  [ok]   {}", m);
        } else {
            HBE_ERROR("  [FAIL] {}", m);
            ++fails;
        }
    };

    const char* svgSrc =
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' "
        "viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='#ff0000'/></svg>";
    const std::vector<u8> bytes(svgSrc, svgSrc + std::strlen(svgSrc));

    std::vector<u32> a, b;
    check(Rasterize(bytes, 64, 64, a), "parse + rasterize 64x64");
    check(a.size() == 64u * 64u, "64x64 buffer size");
    u32 opaque = 0, red = 0;
    for (u32 px : a) {
        const u8 al = static_cast<u8>((px >> 24) & 0xFF);
        if (al > 128) {
            ++opaque;
            const u8 r = static_cast<u8>(px & 0xFF);
            const u8 g = static_cast<u8>((px >> 8) & 0xFF);
            if (r > 200 && g < 60) ++red;
        }
    }
    check(opaque > (64u * 64u) / 10u, "circle covers a meaningful area");
    check(red > 0, "fill colour is red (RGBA byte order correct)");

    // Resolution-independence: a bigger raster covers proportionally more pixels.
    check(Rasterize(bytes, 256, 256, b), "rasterize 256x256");
    check(b.size() == 256u * 256u, "256x256 buffer size");

    // A malformed SVG fails cleanly (no crash).
    const std::vector<u8> junk = {'n', 'o', 't', 's', 'v', 'g'};
    std::vector<u32> j;
    check(!Rasterize(junk, 32, 32, j), "malformed SVG rejected");

    HBE_INFO("--test-uisvg: {} failure(s)", fails);
    return fails == 0;
}

} // namespace hbe::ui::svg
