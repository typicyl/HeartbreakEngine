// UI/FontAtlas.h - the UI text facade.
//
// A FontAtlas is one font FACE (system UI font, or an imported `.uaf` Font asset).
// Layout() shapes UTF-8 text into positioned GlyphQuads via FreeType (rasterize) +
// HarfBuzz (shape) + SheenBidi (bidi), rasterizing on demand into a shared, paged
// glyph atlas (UI/Text/GlyphAtlas). This header stays free of FreeType/HarfBuzz
// types: the face is held behind a forward-declared `text::FontFace` pimpl, so the
// heavy text stack never leaks into the engine's include graph (only FontAtlas.cpp
// and the UI/Text/*.cpp pull it in).
//
// The output GlyphQuad carries a per-GLYPH atlas index, because with a dynamic
// paged atlas different glyphs (and fallback faces) can live on different pages -
// a whole string is no longer one texture. It also carries the source byte cluster
// (caret/selection mapping) and an optional per-glyph colour (rich text).
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hbe {

class Renderer;

namespace ui {

namespace text {
class FontFace;
// Headless text-stack self-test (--test-uitext). Declared here (a FreeType-free
// header) so main_editor.cpp can call it without pulling in the FT/HarfBuzz headers
// that UI/Text/TextShaper.h includes. Defined in TextShaper.cpp.
bool TextSelfTest();
}

// One positioned glyph. Rect + UVs place it; `atlas` is the bindless texture index
// of the glyph atlas PAGE it lives on (0 = none / headless); `cluster` is the source
// UTF-8 byte offset (for caret/selection hit-testing); `color`'s alpha <= 0 means
// "inherit the element colour" (rich-text spans override it).
struct GlyphQuad {
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; // canvas-space rect (origin = layout top-left)
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0; // atlas-page UVs
    u32 atlas = 0;                       // bindless atlas-page texture index
    u32 cluster = 0;                     // source byte offset
    glm::vec4 color{0.0f};               // per-glyph colour; a<=0 = inherit
};

class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // Loads the OS UI font on first call (idempotent).
    bool Initialize(Renderer& renderer);
    // Loads from raw TTF/OTF bytes (an imported Font asset).
    bool InitializeFromMemory(Renderer& renderer, const std::vector<u8>& font);
    bool Ready() const;
    // A STABLE per-face id (nonzero when ready). No longer a single atlas texture -
    // it is used as a glyph-cache key and a readiness marker; emission reads each
    // GlyphQuad's own `atlas` index instead.
    u32 TextureIndex() const { return id_; }

    // Shapes UTF-8 text (handles '\n') at `sizePx` glyph height. Quads are relative
    // to a (0,0) top-left origin; outWidth/outHeight receive the block size.
    void Layout(const std::string& text, f32 sizePx, std::vector<GlyphQuad>& out,
                f32& outWidth, f32& outHeight) const;

    // Block width/height at `sizePx` (no quad output).
    void Measure(const std::string& text, f32 sizePx, f32& outWidth,
                 f32& outHeight) const;

    // Word-wrapped layout: greedily breaks on spaces so no line exceeds `wrapWidth`
    // canvas px (existing '\n' still forces a break). wrapWidth <= 0 == Layout.
    void LayoutWrapped(const std::string& text, f32 sizePx, f32 wrapWidth,
                       std::vector<GlyphQuad>& out, f32& outWidth,
                       f32& outHeight) const;

    // Face used for glyphs this face lacks (per-glyph fallback), e.g. the system
    // font behind an imported asset font.
    void SetFallback(FontAtlas* fb) { fallback_ = fb; }

private:
    void BuildChain(std::vector<text::FontFace*>& chain) const;

    std::unique_ptr<text::FontFace> face_;
    FontAtlas* fallback_ = nullptr;
    Renderer* renderer_ = nullptr; // for the shared atlas' GPU uploads during Layout
    u32 id_ = 0;
    bool failed_ = false;
};

// The default (system-font) atlas used when an element names no font asset.
FontAtlas& SharedFont();

// Font library: per-asset atlases for imported `.uaf` Font assets, baked lazily and
// cached. `rel` is the Assets-relative path (empty = SharedFont). Falls back to the
// system font when the asset is missing/corrupt.
FontAtlas& ResolveFont(Renderer& renderer, const std::filesystem::path& assetsDir,
                       const std::string& rel);

// Drops the cached font-asset atlases (project switch / font re-import).
void ClearFontCache();

} // namespace ui
} // namespace hbe
