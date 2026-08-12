// UI/Text/GlyphAtlas.h - a shared, paged, dynamically-grown glyph atlas.
//
// Rasterized glyph coverage bitmaps (from FreeType, handed in as 8-bit gray) are
// packed into RGBA pages: WHITE rgb + coverage in ALPHA, so the one UI shader
// (`input.color * tex`, Shaders/UI.hlsl) tints text by the vertex colour exactly
// as it did for the old single ASCII atlas - a solid quad, an image and a glyph
// all share one bindless pipeline.
//
// Pages are created on demand and NEVER freed: a page's bindless texture index is
// stable for the process lifetime, so an atlas index baked into a cached UI vertex
// (UISystem's per-element glyph cache) stays valid. One atlas serves every face and
// every pixel size. Growth is a plain shelf packer - glyphs of one size are near-
// uniform height, so shelves waste little, and it needs no third-party packer.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h" // rhi::TextureHandle

#include <unordered_map>
#include <vector>

namespace hbe {
class Renderer;
}

namespace hbe::ui::text {

// Identifies a rasterized glyph: a face id, the FreeType glyph INDEX (shaping has
// already resolved codepoints -> glyph indices, so this is not a codepoint), and
// the integer pixel size it was rendered at.
struct GlyphKey {
    u32 face = 0;
    u32 gid = 0;
    u32 px = 0;
    bool operator==(const GlyphKey& o) const {
        return face == o.face && gid == o.gid && px == o.px;
    }
};

struct GlyphKeyHash {
    usize operator()(const GlyphKey& k) const {
        u64 h = 1469598103934665603ull;
        h = (h ^ k.face) * 1099511628211ull;
        h = (h ^ k.gid) * 1099511628211ull;
        h = (h ^ k.px) * 1099511628211ull;
        return static_cast<usize>(h);
    }
};

// Where a packed glyph lives. UVs are in [0,1] of its atlas page. A whitespace or
// empty glyph is stored with w==0 || h==0 and no packed rect (advance only).
struct GlyphInfo {
    u32 page = 0;        // internal page index (NOT the bindless texture index)
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    i32 w = 0, h = 0;    // bitmap size in px
    i32 bearingLeft = 0; // FT bitmap_left: pen-x -> glyph left edge
    i32 bearingTop = 0;  // FT bitmap_top: baseline -> glyph top edge (up-positive)
};

class GlyphAtlas {
public:
    // The Renderer that owns the bindless table; without one the atlas still packs
    // and reports metrics (headless tests) but every page's texture index stays 0.
    void SetRenderer(Renderer* r) { renderer_ = r; }

    const GlyphInfo* Find(const GlyphKey& k) const;

    // Packs an 8-bit coverage bitmap (row-major, `pitch` bytes per row; `pitch` may
    // exceed `w`). Returns the stored info, which is also stored for w==0 || h==0
    // (whitespace) with no packed rect. GPU upload is deferred to Flush().
    const GlyphInfo& Add(const GlyphKey& k, const u8* gray, i32 w, i32 h, i32 pitch,
                         i32 bearingLeft, i32 bearingTop);

    // Creates/updates GPU textures for pages touched since the last Flush. No-op
    // without a Renderer.
    void Flush();

    // Bindless texture index of a page's GPU texture (0 = not uploaded / headless).
    u32 PageTexture(u32 page) const;

    // Drop everything (project switch / device recreate). Bindless slots of freed
    // pages are intentionally leaked, matching the engine-wide no-free texture policy.
    void Clear();

private:
    static constexpr i32 kPageDim = 1024;
    static constexpr i32 kPad = 1; // 1px gutter so bilinear taps don't bleed neighbours

    struct Page {
        std::vector<u32> pixels;   // kPageDim*kPageDim RGBA (white rgb + coverage a)
        i32 cursorX = kPad;        // shelf packer cursor
        i32 cursorY = kPad;
        i32 shelfH = 0;            // tallest glyph on the current shelf
        rhi::TextureHandle handle; // GPU texture (invalid until first Flush)
        bool dirty = false;        // CPU pixels changed since last upload
    };

    u32 NewPage();                 // returns the new page index

    Renderer* renderer_ = nullptr;
    std::vector<Page> pages_;      // page INDEX is stable (GlyphInfo stores it)
    std::unordered_map<GlyphKey, GlyphInfo, GlyphKeyHash> glyphs_;
};

// The one process-wide atlas shared by every face and size.
GlyphAtlas& SharedGlyphAtlas();

} // namespace hbe::ui::text
