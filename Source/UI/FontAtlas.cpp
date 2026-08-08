// UI/FontAtlas.cpp
#include "UI/FontAtlas.h"

#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Core/Platform.h" // SystemUiFontCandidates (no more hardcoded C:\Windows\Fonts)
#include "Renderer/Renderer.h"
#include "RHI/RHI.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <glm/glm.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace hbe::ui {

namespace fs = std::filesystem;

namespace {

constexpr int kAtlasDim = 1024;
constexpr int kFirstChar = 32;  // space
constexpr int kCharCount = 95;  // ASCII 32..126

std::vector<u8> ReadFontFile() {
    // The OS-provided UI faces, in preference order, resolved from the actual Windows/Fonts
    // directory rather than a hardcoded C: path (which was wrong on non-C: system drives).
    for (const std::filesystem::path& path : platform::SystemUiFontCandidates()) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) continue;
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<u8> bytes(static_cast<usize>(size));
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (in) return bytes;
    }
    return {};
}

} // namespace

FontAtlas& SharedFont() {
    static FontAtlas atlas;
    return atlas;
}

bool FontAtlas::Initialize(Renderer& renderer) {
    if (textureIndex_ != 0) return true;
    if (failed_) return false;
    const std::vector<u8> font = ReadFontFile();
    if (font.empty()) {
        failed_ = true;
        HBE_WARN("UI: no system font found; UI text disabled.");
        return false;
    }
    return InitializeFromMemory(renderer, font);
}

bool FontAtlas::InitializeFromMemory(Renderer& renderer, const std::vector<u8>& font) {
    if (textureIndex_ != 0) return true;
    if (failed_) return false;
    failed_ = true; // assume failure until the upload succeeds
    if (font.empty()) return false;

    // Bake the ASCII range with 2x oversampling for crisp downscaling.
    std::vector<u8> alpha(static_cast<usize>(kAtlasDim) * kAtlasDim);
    packedChars_.resize(sizeof(stbtt_packedchar) * kCharCount);
    stbtt_pack_context pack;
    if (!stbtt_PackBegin(&pack, alpha.data(), kAtlasDim, kAtlasDim, 0, 1, nullptr)) {
        return false;
    }
    stbtt_PackSetOversampling(&pack, 2, 2);
    const int packed = stbtt_PackFontRange(
        &pack, font.data(), 0, bakedSize_, kFirstChar, kCharCount,
        reinterpret_cast<stbtt_packedchar*>(packedChars_.data()));
    stbtt_PackEnd(&pack);
    if (!packed) {
        HBE_WARN("UI: font packing failed; UI text disabled.");
        return false;
    }

    // Vertical metrics (baseline placement + line spacing) at the baked size.
    stbtt_fontinfo info;
    if (stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0))) {
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        const f32 scale = stbtt_ScaleForPixelHeight(&info, bakedSize_);
        ascent_ = ascent * scale;
        lineAdvance_ = (ascent - descent + lineGap) * scale;
    } else {
        ascent_ = bakedSize_ * 0.8f;
        lineAdvance_ = bakedSize_ * 1.2f;
    }

    // White RGBA with the glyph coverage in alpha (color comes from vertices).
    std::vector<u32> pixels(static_cast<usize>(kAtlasDim) * kAtlasDim);
    for (usize i = 0; i < pixels.size(); ++i) {
        pixels[i] = 0x00FFFFFFu | (static_cast<u32>(alpha[i]) << 24);
    }
    rhi::TextureDesc desc;
    desc.width = kAtlasDim;
    desc.height = kAtlasDim;
    desc.format = rhi::Format::R8G8B8A8_UNORM;
    desc.pixels = pixels.data();
    desc.debugName = "ui_font_atlas";
    const rhi::TextureHandle handle = renderer.UploadTexture(desc);
    if (!handle.IsValid()) return false;

    textureIndex_ = handle.index;
    failed_ = false;
    HBE_INFO("UI: font atlas ready ({}x{}, {} glyphs, bindless slot {}).", kAtlasDim,
             kAtlasDim, kCharCount, textureIndex_);
    return true;
}

void FontAtlas::Layout(const std::string& text, f32 sizePx, std::vector<GlyphQuad>& out,
                       f32& outWidth, f32& outHeight) const {
    out.clear();
    outWidth = 0.0f;
    outHeight = 0.0f;
    if (textureIndex_ == 0 || text.empty() || sizePx <= 0.0f) return;

    const auto* chars = reinterpret_cast<const stbtt_packedchar*>(packedChars_.data());
    const f32 scale = sizePx / bakedSize_;
    f32 x = 0.0f;
    f32 baseline = ascent_; // baked-size units; scaled on output
    f32 maxX = 0.0f;

    for (const char ch : text) {
        if (ch == '\n') {
            maxX = glm::max(maxX, x);
            x = 0.0f;
            baseline += lineAdvance_;
            continue;
        }
        const int c = static_cast<unsigned char>(ch);
        if (c < kFirstChar || c >= kFirstChar + kCharCount) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(chars, kAtlasDim, kAtlasDim, c - kFirstChar, &x, &baseline,
                            &q, 0 /*no integer align*/);
        out.push_back({q.x0 * scale, q.y0 * scale, q.x1 * scale, q.y1 * scale,
                       q.s0, q.t0, q.s1, q.t1});
        // stbtt_GetPackedQuad advances x/baseline in baked units; the quad
        // coordinates above already include them.
    }
    maxX = glm::max(maxX, x);
    outWidth = maxX * scale;
    outHeight = (baseline + (lineAdvance_ - ascent_)) * scale;
}

void FontAtlas::Measure(const std::string& text, f32 sizePx, f32& outWidth,
                        f32& outHeight) const {
    static thread_local std::vector<GlyphQuad> scratch;
    Layout(text, sizePx, scratch, outWidth, outHeight);
}

void FontAtlas::LayoutWrapped(const std::string& text, f32 sizePx, f32 wrapWidth,
                              std::vector<GlyphQuad>& out, f32& outWidth,
                              f32& outHeight) const {
    if (wrapWidth <= 0.0f || text.empty()) {
        Layout(text, sizePx, out, outWidth, outHeight);
        return;
    }
    // Assemble a wrapped string (spaces collapsed to one; existing '\n' kept),
    // then reuse Layout's newline handling. Word widths come from Measure.
    f32 spaceW = 0.0f, dummy = 0.0f;
    Measure(" ", sizePx, spaceW, dummy);
    std::string wrapped;
    wrapped.reserve(text.size() + 16);
    f32 lineW = 0.0f;
    bool firstOnLine = true;
    usize i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            wrapped += '\n';
            lineW = 0.0f;
            firstOnLine = true;
            ++i;
            continue;
        }
        if (text[i] == ' ') { ++i; continue; } // run of spaces -> single break point
        usize j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
        const std::string word = text.substr(i, j - i);
        f32 wordW = 0.0f;
        Measure(word, sizePx, wordW, dummy);
        if (!firstOnLine && lineW + spaceW + wordW > wrapWidth) {
            wrapped += '\n';   // wrap before this word
            wrapped += word;
            lineW = wordW;
        } else {
            if (!firstOnLine) { wrapped += ' '; lineW += spaceW; }
            wrapped += word;
            lineW += wordW;
        }
        firstOnLine = false;
        i = j;
    }
    Layout(wrapped, sizePx, out, outWidth, outHeight);
}

// --- Font library -------------------------------------------------------------

namespace {
std::unordered_map<std::string, std::unique_ptr<FontAtlas>>& FontCache() {
    static std::unordered_map<std::string, std::unique_ptr<FontAtlas>> cache;
    return cache;
}
} // namespace

FontAtlas& ResolveFont(Renderer& renderer, const fs::path& assetsDir,
                       const std::string& rel) {
    if (rel.empty()) {
        SharedFont().Initialize(renderer);
        return SharedFont();
    }
    auto& cache = FontCache();
    if (auto it = cache.find(rel); it != cache.end()) {
        // Failed bakes fall back to the system font.
        return it->second->Ready() ? *it->second : SharedFont();
    }
    auto atlas = std::make_unique<FontAtlas>();
    if (const auto ttf = uaf::ReadFont(assetsDir / rel)) {
        atlas->InitializeFromMemory(renderer, *ttf);
    } else {
        HBE_WARN("UI: font asset '{}' missing/corrupt; using the default font.", rel);
    }
    FontAtlas& result = *atlas;
    cache.emplace(rel, std::move(atlas));
    if (!result.Ready()) {
        SharedFont().Initialize(renderer);
        return SharedFont();
    }
    return result;
}

void ClearFontCache() {
    FontCache().clear();
}

} // namespace hbe::ui
