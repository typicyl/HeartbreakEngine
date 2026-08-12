// UI/Text/TextShaper.h - FreeType + HarfBuzz + SheenBidi text shaping.
//
// INTERNAL header: it pulls in the FreeType/HarfBuzz types, so only .cpp files
// (FontAtlas.cpp, TextShaper.cpp, the text self-test) include it - never a public
// UI header. FontAtlas.h forward-declares `text::FontFace` and stores it by
// unique_ptr, so the FT/HB dependency never leaks into the engine's include graph.
//
// Pipeline (ShapeText):
//   UTF-8  ->  SheenBidi (paragraph levels + per-line visual run order)
//          ->  itemize each run into face sub-runs (per-glyph font fallback)
//          ->  HarfBuzz shape each sub-run (kerning/ligatures/complex scripts)
//          ->  rasterize glyph indices through the shared FreeType-backed atlas
//          ->  emit GlyphQuads (position from HarfBuzz, uv/bitmap from the atlas).
#pragma once

#include "Core/Types.h"
#include "UI/FontAtlas.h" // ui::GlyphQuad (the shared output type)

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include <string>
#include <vector>

namespace hbe {
class Renderer;
}

namespace hbe::ui::text {

// One loaded font face: FreeType (rasterization + cmap + metrics) and HarfBuzz
// (shaping) over the SAME in-memory font blob. HarfBuzz is built without FreeType
// integration, so the two parse the blob independently and cooperate only through
// this class - HarfBuzz decides where each glyph goes, FreeType what it looks like.
class FontFace {
public:
    FontFace() = default;
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    // Takes ownership of `blob` (FreeType and HarfBuzz both reference it for life).
    // `id` is the stable atlas/cache key for this face (see FontAtlas).
    bool Load(std::vector<u8> blob, u32 id);
    bool Ready() const { return ready_; }
    u32 Id() const { return id_; }

    FT_Face Ft() const { return ftFace_; }
    hb_font_t* Hb() const { return hbFont_; }

    // FreeType glyph index for a Unicode codepoint (0 = face lacks it) - the font
    // fallback decision.
    u32 GlyphIndex(u32 codepoint) const;

    struct Metrics {
        f32 ascent = 0;     // baseline -> top, px (positive)
        f32 descent = 0;    // baseline -> bottom, px (positive)
        f32 lineHeight = 0; // baseline-to-baseline, px
    };
    Metrics MetricsAt(u32 px) const;

private:
    std::vector<u8> blob_;
    FT_Face ftFace_ = nullptr;
    hb_face_t* hbFace_ = nullptr;
    hb_font_t* hbFont_ = nullptr;
    u32 id_ = 0;
    bool ready_ = false;
};

// Shape `text` with `faces` (index 0 = primary; the rest are the fallback chain,
// tried in order per glyph), rasterizing into the shared atlas via `renderer`
// (may be null - metrics still resolve, atlas indices stay 0). Appends GlyphQuads
// to `out` (cleared first). `wrapWidth > 0` greedily word-wraps to that width in
// canvas px; '\n' always breaks. Positions are relative to a (0,0) top-left origin;
// `outWidth`/`outHeight` receive the block size.
void ShapeText(const std::vector<FontFace*>& faces, const std::string& text,
               f32 pixelSize, f32 wrapWidth, Renderer* renderer,
               std::vector<GlyphQuad>& out, f32& outWidth, f32& outHeight);

// The shared FreeType library (initialized on first use).
FT_Library SharedLibrary();

// TextSelfTest() (--test-uitext) is declared in the FreeType-free UI/FontAtlas.h so
// main_editor.cpp can call it without including the FT/HarfBuzz headers; it is
// defined in TextShaper.cpp.

} // namespace hbe::ui::text
