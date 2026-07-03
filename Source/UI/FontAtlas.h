// UI/FontAtlas.h - TTF font atlas for in-game UI text (stb_truetype).
//
// Bakes a system font (Segoe UI, falling back to Arial) into an RGBA atlas at
// a high pixel size and uploads it into the bindless table once; Layout() then
// emits screen-space glyph quads (atlas UVs + the atlas texture index) for any
// requested size. One global atlas serves all UI text.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {

class Renderer;

namespace ui {

struct GlyphQuad {
    f32 x0, y0, x1, y1; // canvas-space rect (origin = layout top-left)
    f32 u0, v0, u1, v1; // atlas UVs
};

class FontAtlas {
public:
    // Bakes + uploads the SYSTEM font on first call (idempotent).
    bool Initialize(Renderer& renderer);
    // Bakes + uploads from raw TTF/OTF bytes (font assets).
    bool InitializeFromMemory(Renderer& renderer, const std::vector<u8>& ttf);
    bool Ready() const { return textureIndex_ != 0; }
    u32 TextureIndex() const { return textureIndex_; }

    // Lays out UTF-8-ish ASCII text (handles '\n') at `sizePx` glyph height.
    // Quads are relative to a (0,0) top-left origin; outSize receives the
    // total block size so callers can center it.
    void Layout(const std::string& text, f32 sizePx, std::vector<GlyphQuad>& out,
                f32& outWidth, f32& outHeight) const;

    // Width/height of a text block at `sizePx` (no quad output).
    void Measure(const std::string& text, f32 sizePx, f32& outWidth,
                 f32& outHeight) const;

    // Word-wrapped layout: greedily breaks on spaces so no line exceeds
    // `wrapWidth` canvas px (existing '\n' still forces a break; a single word
    // wider than the limit overflows its own line). Delegates to Layout on the
    // assembled multi-line string. wrapWidth <= 0 behaves like Layout.
    void LayoutWrapped(const std::string& text, f32 sizePx, f32 wrapWidth,
                       std::vector<GlyphQuad>& out, f32& outWidth,
                       f32& outHeight) const;

private:
    u32 textureIndex_ = 0;
    bool failed_ = false;
    f32 bakedSize_ = 48.0f;  // px the atlas was baked at
    f32 ascent_ = 0.0f;      // baseline offset at the baked size
    f32 lineAdvance_ = 0.0f; // baseline-to-baseline at the baked size
    std::vector<u8> packedChars_; // stbtt_packedchar[95] (ASCII 32..126)
};

// The default (system-font) atlas used when an element names no font asset.
FontAtlas& SharedFont();

// Font library: per-asset atlases for imported `.uaf` Font assets, baked
// lazily and cached. `rel` is the Assets-relative path (empty = SharedFont).
// Falls back to the system font when the asset is missing/corrupt.
FontAtlas& ResolveFont(Renderer& renderer, const std::filesystem::path& assetsDir,
                       const std::string& rel);

// Drops the cached font-asset atlases (project switch / font re-import).
void ClearFontCache();

} // namespace ui
} // namespace hbe
