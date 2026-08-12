// UI/Text/GlyphAtlas.cpp
#include "UI/Text/GlyphAtlas.h"

#include "Renderer/Renderer.h"

#include <cstring>

namespace hbe::ui::text {

const GlyphInfo* GlyphAtlas::Find(const GlyphKey& k) const {
    auto it = glyphs_.find(k);
    return it == glyphs_.end() ? nullptr : &it->second;
}

u32 GlyphAtlas::NewPage() {
    Page page;
    page.pixels.assign(static_cast<usize>(kPageDim) * kPageDim, 0x00FFFFFFu); // transparent white
    pages_.push_back(std::move(page));
    return static_cast<u32>(pages_.size() - 1);
}

const GlyphInfo& GlyphAtlas::Add(const GlyphKey& k, const u8* gray, i32 w, i32 h,
                                 i32 pitch, i32 bearingLeft, i32 bearingTop) {
    GlyphInfo info;
    info.w = w;
    info.h = h;
    info.bearingLeft = bearingLeft;
    info.bearingTop = bearingTop;

    // Whitespace / empty glyph: no rect to pack; advance-only.
    if (w <= 0 || h <= 0 || gray == nullptr) {
        info.page = pages_.empty() ? 0 : 0;
        auto [it, _] = glyphs_.emplace(k, info);
        return it->second;
    }

    // Find a page with room on the current or a fresh shelf; open a new page if none.
    u32 pageIndex = 0;
    bool placed = false;
    for (u32 p = 0; p < pages_.size() && !placed; ++p) {
        Page& page = pages_[p];
        i32 needW = w + kPad, needH = h + kPad;
        if (page.cursorX + needW > kPageDim) { // wrap to a new shelf
            page.cursorX = kPad;
            page.cursorY += page.shelfH;
            page.shelfH = 0;
        }
        if (page.cursorY + needH > kPageDim) continue; // page full
        pageIndex = p;
        placed = true;
    }
    if (!placed) {
        pageIndex = NewPage();
    }

    Page& page = pages_[pageIndex];
    const i32 x = page.cursorX;
    const i32 y = page.cursorY;
    // Blit coverage into the page: white rgb, gray -> alpha.
    for (i32 row = 0; row < h; ++row) {
        const u8* src = gray + static_cast<i64>(row) * pitch;
        u32* dst = page.pixels.data() + static_cast<usize>(y + row) * kPageDim + x;
        for (i32 col = 0; col < w; ++col) {
            dst[col] = 0x00FFFFFFu | (static_cast<u32>(src[col]) << 24);
        }
    }
    page.cursorX += w + kPad;
    page.shelfH = page.shelfH > (h + kPad) ? page.shelfH : (h + kPad);
    page.dirty = true;

    info.page = pageIndex;
    const f32 inv = 1.0f / static_cast<f32>(kPageDim);
    info.u0 = static_cast<f32>(x) * inv;
    info.v0 = static_cast<f32>(y) * inv;
    info.u1 = static_cast<f32>(x + w) * inv;
    info.v1 = static_cast<f32>(y + h) * inv;

    auto [it, _] = glyphs_.emplace(k, info);
    return it->second;
}

void GlyphAtlas::Flush() {
    if (!renderer_) return;
    for (Page& page : pages_) {
        if (!page.dirty) continue;
        rhi::TextureDesc desc;
        desc.width = kPageDim;
        desc.height = kPageDim;
        desc.format = rhi::Format::R8G8B8A8_UNORM;
        desc.pixels = page.pixels.data();
        desc.debugName = "ui_glyph_atlas_page";
        if (!page.handle.IsValid()) {
            page.handle = renderer_->UploadTexture(desc);
        } else {
            renderer_->UpdateTexture(page.handle, desc);
        }
        page.dirty = false;
    }
}

u32 GlyphAtlas::PageTexture(u32 page) const {
    return page < pages_.size() ? pages_[page].handle.index : 0u;
}

void GlyphAtlas::Clear() {
    pages_.clear();
    glyphs_.clear();
}

GlyphAtlas& SharedGlyphAtlas() {
    static GlyphAtlas atlas;
    return atlas;
}

} // namespace hbe::ui::text
