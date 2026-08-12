// UI/Text/TextShaper.cpp
#include "UI/Text/TextShaper.h"

#include "UI/Text/GlyphAtlas.h"
#include "Core/Log.h"
#include "Core/Platform.h" // SystemUiFontCandidates (--test-uitext)
#include "Renderer/Renderer.h"

#include <hb-ot.h>

// SheenBidi's public headers carry NO `extern "C"` guard, so a C++ TU would mangle
// its symbols and fail to link against the C-compiled sheenbidi.lib. Force C linkage.
extern "C" {
#include <SheenBidi.h>
}

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace hbe::ui::text {

// --- FreeType library ---------------------------------------------------------

FT_Library SharedLibrary() {
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        if (FT_Init_FreeType(&l) != 0) {
            HBE_ERROR("UI text: FT_Init_FreeType failed; text disabled.");
            return static_cast<FT_Library>(nullptr);
        }
        return l;
    }();
    return lib;
}

// --- FontFace -----------------------------------------------------------------

FontFace::~FontFace() {
    if (hbFont_) hb_font_destroy(hbFont_);
    if (hbFace_) hb_face_destroy(hbFace_);
    if (ftFace_) FT_Done_Face(ftFace_);
}

bool FontFace::Load(std::vector<u8> blob, u32 id) {
    blob_ = std::move(blob);
    id_ = id;
    if (blob_.empty()) return false;
    FT_Library lib = SharedLibrary();
    if (!lib) return false;
    if (FT_New_Memory_Face(lib, blob_.data(), static_cast<FT_Long>(blob_.size()), 0,
                           &ftFace_) != 0) {
        ftFace_ = nullptr;
        return false;
    }
    // Prefer a Unicode charmap (some fonts default to a symbol/Mac map).
    FT_Select_Charmap(ftFace_, FT_ENCODING_UNICODE);

    // HarfBuzz parses the SAME blob independently (built without FreeType support).
    hb_blob_t* hbBlob = hb_blob_create(reinterpret_cast<const char*>(blob_.data()),
                                       static_cast<unsigned>(blob_.size()),
                                       HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hbFace_ = hb_face_create(hbBlob, 0);
    hb_blob_destroy(hbBlob);
    if (!hbFace_) return false;
    hbFont_ = hb_font_create(hbFace_);
    if (!hbFont_) return false;
    hb_ot_font_set_funcs(hbFont_); // shape from the OpenType tables (no FT funcs)
    ready_ = true;
    return true;
}

u32 FontFace::GlyphIndex(u32 codepoint) const {
    if (!ftFace_) return 0;
    return FT_Get_Char_Index(ftFace_, codepoint);
}

FontFace::Metrics FontFace::MetricsAt(u32 px) const {
    Metrics m;
    if (!ftFace_ || px == 0) return m;
    FT_Set_Pixel_Sizes(ftFace_, 0, px);
    const FT_Size_Metrics& sm = ftFace_->size->metrics;
    m.ascent = static_cast<f32>(sm.ascender) / 64.0f;
    m.descent = static_cast<f32>(-sm.descender) / 64.0f;
    m.lineHeight = static_cast<f32>(sm.height) / 64.0f;
    if (m.lineHeight <= 0.0f) m.lineHeight = m.ascent + m.descent;
    return m;
}

// --- shaping helpers ----------------------------------------------------------

namespace {

// Decode one UTF-8 codepoint at byte `i`; returns bytes consumed (>=1).
usize DecodeUtf8(const char* s, usize len, usize i, u32& cp) {
    const u8 c = static_cast<u8>(s[i]);
    if (c < 0x80) { cp = c; return 1; }
    if ((c >> 5) == 0x6 && i + 1 < len) {
        cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
        return 2;
    }
    if ((c >> 4) == 0xE && i + 2 < len) {
        cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) |
             (static_cast<u8>(s[i + 2]) & 0x3Fu);
        return 3;
    }
    if ((c >> 3) == 0x1E && i + 3 < len) {
        cp = ((c & 0x07u) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) |
             ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) |
             (static_cast<u8>(s[i + 3]) & 0x3Fu);
        return 4;
    }
    cp = 0xFFFD;
    return 1;
}

struct SubRun {
    u32 face = 0;   // index into the faces chain
    usize off = 0;  // absolute byte offset
    usize len = 0;
};

// Split [off, off+len) into runs of a single face, choosing per codepoint the first
// face in the chain that has a glyph for it (font fallback); primary if none does.
std::vector<SubRun> Itemize(const std::vector<FontFace*>& faces, const std::string& text,
                            usize off, usize len) {
    std::vector<SubRun> runs;
    const usize end = off + len;
    usize i = off;
    int curFace = -1;
    usize runStart = off;
    while (i < end) {
        u32 cp = 0;
        const usize adv = DecodeUtf8(text.data(), text.size(), i, cp);
        int chosen = 0;
        for (usize k = 0; k < faces.size(); ++k) {
            if (faces[k] && faces[k]->Ready() && faces[k]->GlyphIndex(cp) != 0) {
                chosen = static_cast<int>(k);
                break;
            }
        }
        if (curFace == -1) {
            curFace = chosen;
            runStart = i;
        } else if (chosen != curFace) {
            runs.push_back({static_cast<u32>(curFace), runStart, i - runStart});
            curFace = chosen;
            runStart = i;
        }
        i += adv;
    }
    if (curFace != -1) runs.push_back({static_cast<u32>(curFace), runStart, end - runStart});
    return runs;
}

// Shape one face sub-run. When `out` is non-null, rasterizes glyphs through the
// shared atlas and appends quads at `penXStart`; always returns the run's advance
// (so measurement passes null and only sums advances). `atlas` page index is stored
// in GlyphQuad.atlas here and converted to a bindless index by the caller post-Flush.
f32 ShapeSubRun(FontFace* face, const std::string& text, usize off, usize len, bool rtl,
                u32 px, f32 baselineY, f32 penXStart, std::vector<GlyphQuad>* out) {
    if (!face || !face->Ready() || len == 0) return 0.0f;
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()),
                       static_cast<unsigned>(off), static_cast<int>(len));
    hb_buffer_guess_segment_properties(buf); // script + language from content
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR); // bidi wins
    hb_font_set_scale(face->Hb(), static_cast<int>(px) * 64, static_cast<int>(px) * 64);
    hb_shape(face->Hb(), buf, nullptr, 0);

    unsigned n = 0;
    const hb_glyph_info_t* gi = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(buf, &n);
    GlyphAtlas& atlas = SharedGlyphAtlas();
    f32 penX = penXStart;
    for (unsigned i = 0; i < n; ++i) {
        const u32 gid = gi[i].codepoint; // glyph index after shaping
        const f32 xadv = static_cast<f32>(gp[i].x_advance) / 64.0f;
        if (out) {
            const GlyphKey key{face->Id(), gid, px};
            const GlyphInfo* info = atlas.Find(key);
            if (!info) {
                FT_Face ft = face->Ft();
                FT_Set_Pixel_Sizes(ft, 0, px);
                if (FT_Load_Glyph(ft, gid, FT_LOAD_RENDER) == 0) {
                    FT_GlyphSlot g = ft->glyph;
                    info = &atlas.Add(key, g->bitmap.buffer,
                                      static_cast<i32>(g->bitmap.width),
                                      static_cast<i32>(g->bitmap.rows), g->bitmap.pitch,
                                      g->bitmap_left, g->bitmap_top);
                } else {
                    info = &atlas.Add(key, nullptr, 0, 0, 0, 0, 0); // cache the miss
                }
            }
            if (info->w > 0 && info->h > 0) {
                const f32 xoff = static_cast<f32>(gp[i].x_offset) / 64.0f;
                const f32 yoff = static_cast<f32>(gp[i].y_offset) / 64.0f;
                GlyphQuad q;
                q.x0 = penX + xoff + static_cast<f32>(info->bearingLeft);
                q.y0 = baselineY - static_cast<f32>(info->bearingTop) - yoff;
                q.x1 = q.x0 + static_cast<f32>(info->w);
                q.y1 = q.y0 + static_cast<f32>(info->h);
                q.u0 = info->u0; q.v0 = info->v0;
                q.u1 = info->u1; q.v1 = info->v1;
                q.atlas = info->page; // page index now; -> bindless index after Flush
                q.cluster = static_cast<u32>(gi[i].cluster);
                q.color = glm::vec4(0.0f); // inherit the element colour
                out->push_back(q);
            }
        }
        penX += xadv;
    }
    hb_buffer_destroy(buf);
    return penX - penXStart;
}

// Emit one bidi run (visual order from SheenBidi): itemize into face sub-runs, and
// for an RTL run place those sub-runs right-to-left so mixed-font RTL stays correct.
void EmitRun(const std::vector<FontFace*>& faces, const std::string& text, usize off,
             usize len, u32 level, u32 px, f32 baselineY, f32& penX,
             std::vector<GlyphQuad>& out) {
    const bool rtl = (level & 1u) != 0;
    std::vector<SubRun> subs = Itemize(faces, text, off, len);
    if (rtl) std::reverse(subs.begin(), subs.end());
    for (const SubRun& sr : subs) {
        FontFace* f = sr.face < faces.size() ? faces[sr.face] : (faces.empty() ? nullptr : faces[0]);
        penX += ShapeSubRun(f, text, sr.off, sr.len, rtl, px, baselineY, penX, &out);
    }
}

// Total advance width of a standalone string (word-wrap measurement).
f32 MeasureText(const std::vector<FontFace*>& faces, const std::string& s, u32 px) {
    if (s.empty()) return 0.0f;
    std::vector<SubRun> subs = Itemize(faces, s, 0, s.size());
    f32 w = 0.0f;
    for (const SubRun& sr : subs) {
        FontFace* f = sr.face < faces.size() ? faces[sr.face] : nullptr;
        w += ShapeSubRun(f, s, sr.off, sr.len, false, px, 0.0f, 0.0f, nullptr);
    }
    return w;
}

// Greedy word-wrap: contiguous byte ranges (offset,len) covering [paraStart,paraEnd).
std::vector<std::pair<usize, usize>> ComputeLineBreaks(const std::vector<FontFace*>& faces,
                                                       const std::string& text,
                                                       usize paraStart, usize paraLen,
                                                       u32 px, f32 wrapWidth) {
    std::vector<std::pair<usize, usize>> lines;
    if (wrapWidth <= 0.0f || paraLen == 0) {
        lines.push_back({paraStart, paraLen});
        return lines;
    }
    const usize end = paraStart + paraLen;
    const f32 spaceW = MeasureText(faces, " ", px);
    usize i = paraStart;
    usize lineStart = paraStart;
    f32 lineW = 0.0f;
    bool firstWord = true;
    while (i < end) {
        if (text[i] == ' ') { ++i; continue; }
        const usize wordOff = i;
        while (i < end && text[i] != ' ') ++i;
        const std::string word = text.substr(wordOff, i - wordOff);
        const f32 ww = MeasureText(faces, word, px);
        if (!firstWord && lineW + spaceW + ww > wrapWidth) {
            lines.push_back({lineStart, wordOff - lineStart});
            lineStart = wordOff;
            lineW = ww;
        } else {
            lineW += (firstWord ? 0.0f : spaceW) + ww;
        }
        firstWord = false;
    }
    lines.push_back({lineStart, end - lineStart});
    return lines;
}

} // namespace

// --- ShapeText ----------------------------------------------------------------

void ShapeText(const std::vector<FontFace*>& faces, const std::string& text, f32 pixelSize,
               f32 wrapWidth, Renderer* renderer, std::vector<GlyphQuad>& out,
               f32& outWidth, f32& outHeight) {
    out.clear();
    outWidth = 0.0f;
    outHeight = 0.0f;
    if (faces.empty() || !faces[0] || !faces[0]->Ready() || text.empty() ||
        pixelSize <= 0.0f)
        return;

    const u32 px = std::max<u32>(1u, static_cast<u32>(std::lround(pixelSize)));
    GlyphAtlas& atlas = SharedGlyphAtlas();
    if (renderer) atlas.SetRenderer(renderer);
    FontFace::Metrics m = faces[0]->MetricsAt(px);
    if (m.lineHeight <= 0.0f) m.lineHeight = static_cast<f32>(px) * 1.2f;

    SBCodepointSequence seq{SBStringEncodingUTF8, const_cast<char*>(text.data()),
                            static_cast<SBUInteger>(text.size())};
    SBAlgorithmRef algo = SBAlgorithmCreate(&seq);

    f32 baselineY = m.ascent;
    f32 maxW = 0.0f;
    int lineCount = 0;
    const usize n = text.size();
    usize paraStart = 0;
    while (paraStart <= n) {
        usize paraEnd = paraStart;
        while (paraEnd < n && text[paraEnd] != '\n') ++paraEnd;
        const usize paraLen = paraEnd - paraStart;

        const auto lines = ComputeLineBreaks(faces, text, paraStart, paraLen, px, wrapWidth);
        SBParagraphRef para =
            (algo && paraLen > 0)
                ? SBAlgorithmCreateParagraph(algo, paraStart, paraLen, SBLevelDefaultLTR)
                : nullptr;

        for (const auto& [loff, llen] : lines) {
            f32 penX = 0.0f;
            if (llen > 0) {
                if (para) {
                    SBLineRef sbline = SBParagraphCreateLine(para, loff, llen);
                    if (sbline) {
                        const SBUInteger rc = SBLineGetRunCount(sbline);
                        const SBRun* runs = SBLineGetRunsPtr(sbline);
                        for (SBUInteger r = 0; r < rc; ++r) {
                            EmitRun(faces, text, runs[r].offset, runs[r].length,
                                    runs[r].level, px, baselineY, penX, out);
                        }
                        SBLineRelease(sbline);
                    } else {
                        EmitRun(faces, text, loff, llen, 0, px, baselineY, penX, out);
                    }
                } else {
                    EmitRun(faces, text, loff, llen, 0, px, baselineY, penX, out);
                }
            }
            maxW = std::max(maxW, penX);
            baselineY += m.lineHeight;
            ++lineCount;
        }
        if (para) SBParagraphRelease(para);

        if (paraEnd >= n) break;
        paraStart = paraEnd + 1; // skip the '\n'
    }
    if (algo) SBAlgorithmRelease(algo);

    atlas.Flush();
    for (GlyphQuad& q : out) q.atlas = atlas.PageTexture(q.atlas); // page -> bindless index

    outWidth = maxW;
    outHeight = static_cast<f32>(lineCount) * m.lineHeight;
}

// --- Self-test (--test-uitext) ------------------------------------------------

namespace {
std::vector<u8> LoadSystemFontBlob() {
    for (const std::filesystem::path& p : platform::SystemUiFontCandidates()) {
        std::ifstream in(p, std::ios::binary | std::ios::ate);
        if (!in) continue;
        const std::streamsize sz = in.tellg();
        in.seekg(0);
        std::vector<u8> b(static_cast<usize>(sz));
        in.read(reinterpret_cast<char*>(b.data()), sz);
        if (in) return b;
    }
    return {};
}
} // namespace

bool TextSelfTest() {
    int failures = 0;
    const auto check = [&](bool cond, const char* msg) {
        if (cond) {
            HBE_INFO("  [ok]   {}", msg);
        } else {
            HBE_ERROR("  [FAIL] {}", msg);
            ++failures;
        }
    };

    std::vector<u8> blob = LoadSystemFontBlob();
    if (blob.empty()) {
        HBE_ERROR("--test-uitext: no system font found");
        return false;
    }
    FontFace face;
    if (!face.Load(std::move(blob), 1)) {
        HBE_ERROR("--test-uitext: FontFace load failed");
        return false;
    }
    std::vector<FontFace*> chain{&face};

    std::vector<GlyphQuad> q;
    f32 w = 0.0f, h = 0.0f;

    // 1. ASCII still renders (5 letters; tolerate a stray ligature).
    ShapeText(chain, "Hello", 32.0f, 0.0f, nullptr, q, w, h);
    check(q.size() >= 4 && q.size() <= 5, "ASCII 'Hello' shapes to ~5 glyphs");
    check(w > 10.0f && h > 10.0f, "ASCII has a sane positive extent");
    const f32 helloW = w;

    // 2. Non-ASCII RENDERS - the OLD ASCII atlas dropped every byte >= 0x80. 'Cafe'
    //    with a combining-free precomposed U+00E9 (UTF-8 C3 A9).
    ShapeText(chain, "Caf\xC3\xA9", 32.0f, 0.0f, nullptr, q, w, h);
    check(q.size() >= 4, "Latin-1 accented glyph renders (>=4 glyphs)");

    // 3. Cyrillic renders (U+041C U+0438 U+0440 = 'Mir').
    ShapeText(chain, "\xD0\x9C\xD0\xB8\xD1\x80", 32.0f, 0.0f, nullptr, q, w, h);
    check(!q.empty(), "Cyrillic renders");

    // 4. Determinism: identical input -> identical width.
    ShapeText(chain, "Hello", 32.0f, 0.0f, nullptr, q, w, h);
    check(std::abs(w - helloW) < 0.01f, "shaping is deterministic");

    // 5. Word-wrap: a small wrap width produces more than one line.
    const char* fox = "the quick brown fox jumps over the lazy dog";
    ShapeText(chain, fox, 24.0f, 0.0f, nullptr, q, w, h);
    const f32 oneLineH = h, fullW = w;
    ShapeText(chain, fox, 24.0f, fullW * 0.4f, nullptr, q, w, h);
    check(h > oneLineH + 1.0f, "wrapWidth wraps to multiple lines");

    // 6. Explicit newline makes two lines.
    ShapeText(chain, "a", 24.0f, 0.0f, nullptr, q, w, h);
    const f32 lineH = h;
    ShapeText(chain, "a\nb", 24.0f, 0.0f, nullptr, q, w, h);
    check(h > lineH + 1.0f, "'\\n' produces a second line");

    // 7. Bidi: RTL text is visually reordered - the visually-LEFTMOST glyph maps to
    //    a LATER source cluster than the rightmost. Hebrew 'shalom' (U+05E9 05DC 05D5 05DD).
    ShapeText(chain, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", 32.0f, 0.0f, nullptr, q, w, h);
    if (q.size() >= 2) {
        usize li = 0, ri = 0;
        for (usize k = 1; k < q.size(); ++k) {
            if (q[k].x0 < q[li].x0) li = k;
            if (q[k].x0 > q[ri].x0) ri = k;
        }
        check(q[li].cluster > q[ri].cluster, "RTL text is visually reordered (bidi)");
    } else {
        HBE_WARN("  [skip] bidi: system font produced < 2 Hebrew glyphs");
    }

    HBE_INFO("--test-uitext: {} failure(s)", failures);
    return failures == 0;
}

} // namespace hbe::ui::text
