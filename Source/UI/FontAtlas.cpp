// UI/FontAtlas.cpp
#include "UI/FontAtlas.h"

#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Core/Platform.h" // SystemUiFontCandidates (no hardcoded C:\Windows\Fonts)
#include "UI/Text/GlyphAtlas.h"
#include "UI/Text/TextShaper.h"

#include <fstream>
#include <unordered_map>

namespace hbe::ui {

namespace fs = std::filesystem;

namespace {

// Monotonic per-face id (stable glyph-cache key + readiness marker). 0 = none.
u32 NextFaceId() {
    static u32 next = 1;
    return next++;
}

std::vector<u8> ReadFontFile() {
    // The OS-provided UI faces, in preference order, resolved from the real
    // Windows/Fonts directory (not a hardcoded C: path).
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

FontAtlas::FontAtlas() = default;
FontAtlas::~FontAtlas() = default;

bool FontAtlas::Ready() const {
    return face_ && face_->Ready();
}

bool FontAtlas::Initialize(Renderer& renderer) {
    if (Ready()) return true;
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
    if (Ready()) return true;
    if (failed_) return false;
    if (font.empty()) {
        failed_ = true;
        return false;
    }
    renderer_ = &renderer;
    text::SharedGlyphAtlas().SetRenderer(&renderer);
    const u32 id = NextFaceId();
    face_ = std::make_unique<text::FontFace>();
    if (!face_->Load(font, id)) { // FontFace copies the blob it is given
        face_.reset();
        failed_ = true;
        HBE_WARN("UI: font face load failed; UI text disabled.");
        return false;
    }
    id_ = id;
    HBE_INFO("UI: font face ready (FreeType+HarfBuzz, id {}).", id_);
    return true;
}

void FontAtlas::BuildChain(std::vector<text::FontFace*>& chain) const {
    chain.clear();
    if (face_) chain.push_back(face_.get());
    // The fallback face (system font) is consulted per glyph the primary lacks.
    if (fallback_ && fallback_ != this && fallback_->Ready())
        chain.push_back(fallback_->face_.get());
}

void FontAtlas::Layout(const std::string& text, f32 sizePx, std::vector<GlyphQuad>& out,
                       f32& outWidth, f32& outHeight) const {
    out.clear();
    outWidth = outHeight = 0.0f;
    if (!Ready()) return;
    std::vector<text::FontFace*> chain;
    BuildChain(chain);
    text::ShapeText(chain, text, sizePx, 0.0f, renderer_, out, outWidth, outHeight);
}

void FontAtlas::Measure(const std::string& text, f32 sizePx, f32& outWidth,
                        f32& outHeight) const {
    static thread_local std::vector<GlyphQuad> scratch;
    Layout(text, sizePx, scratch, outWidth, outHeight);
}

void FontAtlas::LayoutWrapped(const std::string& text, f32 sizePx, f32 wrapWidth,
                              std::vector<GlyphQuad>& out, f32& outWidth,
                              f32& outHeight) const {
    out.clear();
    outWidth = outHeight = 0.0f;
    if (!Ready()) return;
    std::vector<text::FontFace*> chain;
    BuildChain(chain);
    text::ShapeText(chain, text, sizePx, wrapWidth, renderer_, out, outWidth, outHeight);
}

// --- Shared + font library ----------------------------------------------------

FontAtlas& SharedFont() {
    static FontAtlas atlas;
    return atlas;
}

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
        return it->second->Ready() ? *it->second : SharedFont();
    }
    auto atlas = std::make_unique<FontAtlas>();
    // Glyphs the asset font lacks fall back to the system font.
    SharedFont().Initialize(renderer);
    atlas->SetFallback(&SharedFont());
    if (const auto ttf = uaf::ReadFont(assetsDir / rel)) {
        atlas->InitializeFromMemory(renderer, *ttf);
    } else {
        HBE_WARN("UI: font asset '{}' missing/corrupt; using the default font.", rel);
    }
    FontAtlas& result = *atlas;
    cache.emplace(rel, std::move(atlas));
    if (!result.Ready()) return SharedFont();
    return result;
}

void ClearFontCache() {
    FontCache().clear();
    // The shared glyph atlas holds bindless pages keyed to the (now stale) device /
    // project; drop them so a project switch re-bakes into fresh slots.
    text::SharedGlyphAtlas().Clear();
}

} // namespace hbe::ui
