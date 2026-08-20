// Material/MaterialGraphCompiler.cpp - see MaterialGraphCompiler.h.
#include "Material/MaterialGraphCompiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hbe::mat {

namespace {

// ---- Deterministic value noise / voronoi (hash-based, portable) --------------------------
u32 Hash2(i32 x, i32 y, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= static_cast<u32>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<u32>(y) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}
f32 Hash01(i32 x, i32 y, u32 seed) { return static_cast<f32>(Hash2(x, y, seed) & 0xFFFFFFu) / 16777215.0f; }

f32 ValueNoise(glm::vec2 p, u32 seed) {
    const i32 x0 = static_cast<i32>(std::floor(p.x));
    const i32 y0 = static_cast<i32>(std::floor(p.y));
    const f32 fx = p.x - static_cast<f32>(x0);
    const f32 fy = p.y - static_cast<f32>(y0);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx);
    const f32 uy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = Hash01(x0, y0, seed), b = Hash01(x0 + 1, y0, seed);
    const f32 c = Hash01(x0, y0 + 1, seed), d = Hash01(x0 + 1, y0 + 1, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

f32 VoronoiF1(glm::vec2 p, u32 seed) {
    const i32 cx = static_cast<i32>(std::floor(p.x));
    const i32 cy = static_cast<i32>(std::floor(p.y));
    f32 best = 8.0f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 gx = cx + dx, gy = cy + dy;
            const glm::vec2 feat(static_cast<f32>(gx) + Hash01(gx, gy, seed),
                                 static_cast<f32>(gy) + Hash01(gx, gy, seed ^ 0x1234u));
            const glm::vec2 d = feat - p;
            best = std::min(best, glm::dot(d, d));
        }
    return std::clamp(std::sqrt(best), 0.0f, 1.0f);
}

// --- Perlin (gradient) noise + FBM -------------------------------------------------------
glm::vec2 Grad2(i32 ix, i32 iy, u32 seed) {
    const f32 a = static_cast<f32>(Hash2(ix, iy, seed) & 0xFFFFu) / 65535.0f * 6.2831853f;
    return glm::vec2(std::cos(a), std::sin(a));
}
f32 Perlin2(glm::vec2 p, u32 seed) {
    const i32 x0 = static_cast<i32>(std::floor(p.x)), y0 = static_cast<i32>(std::floor(p.y));
    const f32 fx = p.x - x0, fy = p.y - y0;
    auto dg = [&](i32 ix, i32 iy, f32 dx, f32 dy) {
        const glm::vec2 g = Grad2(ix, iy, seed);
        return g.x * dx + g.y * dy;
    };
    const f32 n00 = dg(x0, y0, fx, fy), n10 = dg(x0 + 1, y0, fx - 1, fy);
    const f32 n01 = dg(x0, y0 + 1, fx, fy - 1), n11 = dg(x0 + 1, y0 + 1, fx - 1, fy - 1);
    const f32 ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
    const f32 uy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    const f32 nx0 = n00 + (n10 - n00) * ux, nx1 = n01 + (n11 - n01) * ux;
    return std::clamp((nx0 + (nx1 - nx0) * uy) * 0.5f + 0.5f, 0.0f, 1.0f); // to [0,1]
}
f32 Fbm2(glm::vec2 p, int octaves, f32 persistence, u32 seed) {
    octaves = std::clamp(octaves, 1, 8);
    if (persistence <= 0.0f) persistence = 0.5f;
    f32 sum = 0.0f, amp = 0.5f, tot = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += Perlin2(p, seed + static_cast<u32>(i)) * amp;
        tot += amp;
        p *= 2.0f;
        amp *= persistence;
    }
    return tot > 0.0f ? sum / tot : 0.0f;
}
// Worley: fills F1/F2 (nearest, 2nd-nearest) and the winning cell coordinate.
void CellF1F2(glm::vec2 p, u32 seed, f32& f1, f32& f2, glm::vec2& cell) {
    const i32 cx = static_cast<i32>(std::floor(p.x)), cy = static_cast<i32>(std::floor(p.y));
    f1 = f2 = 8.0f;
    cell = glm::vec2(0.0f);
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 gx = cx + dx, gy = cy + dy;
            const glm::vec2 feat(static_cast<f32>(gx) + Hash01(gx, gy, seed),
                                 static_cast<f32>(gy) + Hash01(gx, gy, seed ^ 0x1234u));
            const f32 d = glm::length(feat - p);
            if (d < f1) { f2 = f1; f1 = d; cell = glm::vec2(gx, gy); }
            else if (d < f2) { f2 = d; }
        }
}

// --- Colour space + SDF helpers ----------------------------------------------------------
glm::vec3 Rgb2Hsv(glm::vec3 c) {
    const f32 mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    const f32 d = mx - mn;
    f32 h = 0.0f;
    if (d > 1e-6f) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
        else h = (c.r - c.g) / d + 4.0f;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    return glm::vec3(h, mx > 0.0f ? d / mx : 0.0f, mx);
}
glm::vec3 Hsv2Rgb(glm::vec3 c) {
    const f32 h = c.x * 6.0f, s = c.y, v = c.z;
    const f32 i = std::floor(h), f = h - i;
    const f32 p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}
f32 Smin(f32 a, f32 b, f32 k) {
    if (k <= 0.0f) return std::min(a, b);
    const f32 h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return glm::mix(b, a, h) - k * h * (1.0f - h);
}
// Regular n-gon signed distance (centered at origin). n<3 -> circle.
f32 SdfPolygon(glm::vec2 p, f32 r, int n) {
    if (n < 3) return glm::length(p) - r;
    const f32 seg = 6.2831853f / static_cast<f32>(n);
    f32 a = std::atan2(p.y, p.x);
    a = std::fmod(a, seg);
    if (a < 0.0f) a += seg;
    a -= seg * 0.5f;
    return std::cos(a) * glm::length(p) - r * std::cos(seg * 0.5f);
}
f32 Frac(f32 v) { return v - std::floor(v); }

// One "Substance-style" blend mode (per component). a = bottom, b = top.
glm::vec3 BlendMode(int mode, glm::vec3 a, glm::vec3 b) {
    switch (mode) {
        case 1: return a * b;                                    // Multiply
        case 2: return 1.0f - (1.0f - a) * (1.0f - b);           // Screen
        case 3: { // Overlay
            glm::vec3 r;
            for (int i = 0; i < 3; ++i)
                r[i] = a[i] < 0.5f ? 2 * a[i] * b[i] : 1 - 2 * (1 - a[i]) * (1 - b[i]);
            return r;
        }
        case 4: return glm::min(a, b);                           // Darken
        case 5: return glm::max(a, b);                           // Lighten
        case 6: return glm::abs(a - b);                          // Difference
        case 7: return glm::min(a + b, glm::vec3(1.0f));         // Add
        case 8: return glm::max(a - b, glm::vec3(0.0f));         // Subtract
        case 9: { // Dodge
            glm::vec3 r;
            for (int i = 0; i < 3; ++i) r[i] = b[i] >= 1.0f ? 1.0f : std::min(1.0f, a[i] / (1 - b[i]));
            return r;
        }
        case 10: { // Burn
            glm::vec3 r;
            for (int i = 0; i < 3; ++i) r[i] = b[i] <= 0.0f ? 0.0f : 1 - std::min(1.0f, (1 - a[i]) / b[i]);
            return r;
        }
        case 11: { // Soft light
            glm::vec3 r;
            for (int i = 0; i < 3; ++i)
                r[i] = b[i] < 0.5f ? a[i] - (1 - 2 * b[i]) * a[i] * (1 - a[i])
                                   : a[i] + (2 * b[i] - 1) * ((a[i] < 0.25f
                                                                  ? ((16 * a[i] - 12) * a[i] + 4) * a[i]
                                                                  : std::sqrt(a[i])) - a[i]);
            return r;
        }
        case 12: { // Hard light (overlay with a,b swapped)
            glm::vec3 r;
            for (int i = 0; i < 3; ++i)
                r[i] = b[i] < 0.5f ? 2 * a[i] * b[i] : 1 - 2 * (1 - a[i]) * (1 - b[i]);
            return r;
        }
        default: return b; // Normal
    }
}

glm::vec3 SpacePos(const SampleContext& ctx, Space s) {
    switch (s) {
        case Space::UV0: return {ctx.uv0.x, ctx.uv0.y, 0.0f};
        case Space::UV1: return {ctx.uv1.x, ctx.uv1.y, 0.0f};
        case Space::Object: return ctx.objectPos;
        case Space::World: return ctx.worldPos;
        case Space::Triplanar: return ctx.worldPos;
        default: return {ctx.uv0.x, ctx.uv0.y, 0.0f};
    }
}
glm::vec2 SpaceCoord2(const SampleContext& ctx, Space s) {
    const glm::vec3 p = SpacePos(ctx, s);
    if (s == Space::World || s == Space::Object || s == Space::Triplanar) return {p.x, p.z};
    return {p.x, p.y};
}

glm::vec4 EvalRamp(const std::vector<RampStop>& ramp, f32 t) {
    if (ramp.empty()) return glm::vec4(t, t, t, 1.0f);
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= ramp.front().pos) return ramp.front().color;
    if (t >= ramp.back().pos) return ramp.back().color;
    for (usize i = 1; i < ramp.size(); ++i) {
        if (t <= ramp[i].pos) {
            const f32 span = ramp[i].pos - ramp[i - 1].pos;
            const f32 f = span > 1e-6f ? (t - ramp[i - 1].pos) / span : 0.0f;
            return glm::mix(ramp[i - 1].color, ramp[i].color, f);
        }
    }
    return ramp.back().color;
}

// Evaluate one VALUE op given its resolved input values (already sampled at this uv). `has[k]` =
// input pin k is connected. Coordinate-transform + resampling nodes are NOT handled here (they
// live in EvalRec, which re-samples inputs at a modified uv); this covers constants, math, texture
// samples, generators (from the sample coord), per-pixel filters, and SDF value ops.
glm::vec4 EvalOpValue(const Op& op, const glm::vec4 in[], const bool has[], const SampleContext& ctx,
                      const TextureProvider& tex) {
    // Coord for generators: an explicit coord input, else the node's space coordinate.
    auto genCoord = [&]() -> glm::vec2 {
        return has[0] ? glm::vec2(in[0]) : SpaceCoord2(ctx, op.space);
    };
    switch (op.type) {
        case NodeType::Constant:
        case NodeType::Color:
        case NodeType::Float:
        case NodeType::Vector:
            return op.constant;

        case NodeType::UV: return {ctx.uv0.x, ctx.uv0.y, 0.0f, 0.0f};
        case NodeType::WorldPosition: return {ctx.worldPos, 1.0f};
        case NodeType::ObjectPosition: return {ctx.objectPos, 1.0f};
        case NodeType::Normal: return {ctx.normal, 0.0f};
        case NodeType::VertexColor: return ctx.vertexColor;

        case NodeType::Texture: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            if (tex) return tex(op.texture, uv, NodeType::Texture);
            return op.constant.x != 0.0f || op.constant.y != 0.0f ? op.constant : glm::vec4(1.0f);
        }
        case NodeType::NormalMap: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const glm::vec4 e = tex ? tex(op.texture, uv, NodeType::NormalMap) : glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
            return {e.x * 2.0f - 1.0f, e.y * 2.0f - 1.0f, e.z * 2.0f - 1.0f, 1.0f};
        }
        case NodeType::Height: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const f32 h = tex ? tex(op.texture, uv, NodeType::Height).x : op.constant.x;
            return glm::vec4(h);
        }
        case NodeType::Mask: {
            const glm::vec2 uv = has[0] ? glm::vec2(in[0]) : ctx.uv0;
            const f32 m = tex ? tex(op.texture, uv, NodeType::Mask).x : op.constant.x;
            return glm::vec4(m);
        }

        case NodeType::Multiply: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(1.0f);
            const glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            return a * b;
        }
        case NodeType::Add: return (has[0] ? in[0] : glm::vec4(0.0f)) + (has[1] ? in[1] : glm::vec4(0.0f));
        case NodeType::Subtract: return (has[0] ? in[0] : glm::vec4(0.0f)) - (has[1] ? in[1] : glm::vec4(0.0f));
        case NodeType::Divide: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(0.0f);
            glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            for (int i = 0; i < 4; ++i) b[i] = std::abs(b[i]) < 1e-8f ? 1.0f : b[i];
            return a / b;
        }
        case NodeType::Lerp: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(0.0f);
            const glm::vec4 b = has[1] ? in[1] : glm::vec4(1.0f);
            const f32 t = has[2] ? in[2].x : op.constant.x;
            return glm::mix(a, b, std::clamp(t, 0.0f, 1.0f));
        }
        case NodeType::Clamp: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            return glm::clamp(x, glm::vec4(op.constant.x), glm::vec4(op.constant.y));
        }
        case NodeType::Remap: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 inSpan = op.constant.y - op.constant.x;
            const f32 outSpan = op.constant.w - op.constant.z;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) {
                const f32 t = std::abs(inSpan) < 1e-8f ? 0.0f : (x[i] - op.constant.x) / inSpan;
                r[i] = op.constant.z + t * outSpan;
            }
            return r;
        }
        case NodeType::Power: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 e = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) {
                f32 base = std::max(x[i], 0.0f);
                if (e < 0.0f) base = std::max(base, 1e-6f); // guard pow(0, negative) = +inf
                r[i] = std::pow(base, e);
            }
            return r;
        }
        case NodeType::Smoothstep: {
            const glm::vec4 x = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 e0 = op.constant.x, e1 = op.constant.y;
            glm::vec4 r;
            for (int i = 0; i < 4; ++i) {
                const f32 span = e1 - e0;
                const f32 t = std::abs(span) < 1e-8f ? (x[i] >= e1 ? 1.0f : 0.0f)
                                                     : std::clamp((x[i] - e0) / span, 0.0f, 1.0f);
                r[i] = t * t * (3.0f - 2.0f * t);
            }
            return r;
        }
        case NodeType::OneMinus: return glm::vec4(1.0f) - (has[0] ? in[0] : glm::vec4(0.0f));

        case NodeType::Noise: {
            const glm::vec2 c = has[0] ? glm::vec2(in[0]) : SpaceCoord2(ctx, op.space);
            const f32 scale = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const u32 seed = static_cast<u32>(op.constant.y) + 1u;
            return glm::vec4(ValueNoise(c * scale, seed));
        }
        case NodeType::Voronoi: {
            const glm::vec2 c = has[0] ? glm::vec2(in[0]) : SpaceCoord2(ctx, op.space);
            const f32 scale = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const u32 seed = static_cast<u32>(op.constant.y) + 1u;
            return glm::vec4(VoronoiF1(c * scale, seed));
        }
        case NodeType::Gradient: {
            const f32 t = has[0] ? in[0].x : SpaceCoord2(ctx, op.space).x;
            const f32 tc = std::clamp(t, 0.0f, 1.0f);
            return {tc, tc, tc, 1.0f};
        }
        case NodeType::ColorRamp: {
            const f32 t = has[0] ? in[0].x : 0.0f;
            return EvalRamp(op.ramp, t);
        }

        case NodeType::MaterialLayer: return op.constant; // external ref; neutral in value eval

        // === Generators (from the sample coordinate) ===
        case NodeType::Perlin: {
            const f32 s = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            return glm::vec4(Perlin2(genCoord() * s, static_cast<u32>(op.constant.y) + 1u));
        }
        case NodeType::FractalNoise: {
            const f32 s = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const int oct = op.constant.y == 0.0f ? 5 : static_cast<int>(op.constant.y);
            return glm::vec4(Fbm2(genCoord() * s, oct, op.constant.z, static_cast<u32>(op.constant.w) + 1u));
        }
        case NodeType::Cellular: {
            const f32 s = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            f32 f1, f2;
            glm::vec2 cell;
            CellF1F2(genCoord() * s, static_cast<u32>(op.constant.y) + 1u, f1, f2, cell);
            f32 v;
            switch (static_cast<int>(op.constant.z)) {
                case 1: v = std::min(f2, 1.0f); break;
                case 2: v = std::clamp(f2 - f1, 0.0f, 1.0f); break;
                case 3: v = Hash01(static_cast<i32>(cell.x), static_cast<i32>(cell.y),
                                   static_cast<u32>(op.constant.y) + 7u); break;
                default: v = std::min(f1, 1.0f); break;
            }
            return glm::vec4(v);
        }
        case NodeType::Checker: {
            const f32 t = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const glm::vec2 c = genCoord() * t;
            const f32 v = ((static_cast<i32>(std::floor(c.x)) + static_cast<i32>(std::floor(c.y))) & 1) ? 1.0f : 0.0f;
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::Bricks: {
            const f32 cols = op.constant.x == 0.0f ? 4.0f : op.constant.x;
            const f32 rows = op.constant.y == 0.0f ? 8.0f : op.constant.y;
            const f32 mortar = op.constant.z == 0.0f ? 0.05f : op.constant.z;
            const f32 rowOff = op.constant.w == 0.0f ? 0.5f : op.constant.w;
            glm::vec2 c = genCoord();
            const f32 row = std::floor(c.y * rows);
            c.x += row * rowOff / cols; // stagger alternating rows
            const f32 bx = Frac(c.x * cols), by = Frac(c.y * rows);
            const f32 mh = mortar * 0.5f; // `mortar` is the TOTAL gap fraction (split both sides)
            const bool inMortar = bx < mh || bx > 1 - mh || by < mh || by > 1 - mh;
            if (inMortar) return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            const f32 rnd = Hash01(static_cast<i32>(std::floor(c.x * cols)), static_cast<i32>(row), 13u);
            const f32 v = 0.5f + 0.5f * rnd;
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::Grid: {
            const f32 t = op.constant.x == 0.0f ? 8.0f : op.constant.x;
            const f32 lw = op.constant.y == 0.0f ? 0.05f : op.constant.y;
            const glm::vec2 c = glm::vec2(Frac(genCoord().x * t), Frac(genCoord().y * t));
            const f32 v = (c.x < lw || c.x > 1 - lw || c.y < lw || c.y > 1 - lw) ? 1.0f : 0.0f;
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::Shape: {
            const int sides = static_cast<int>(op.constant.x);
            const f32 radius = op.constant.y == 0.0f ? 0.4f : op.constant.y;
            const glm::vec2 q = genCoord() - 0.5f;
            const f32 d = SdfPolygon(q, radius, sides);
            const f32 aa = op.constant.z > 0.0f ? op.constant.z : 0.01f;
            const f32 v = 1.0f - glm::smoothstep(-aa, aa, d);
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::Wave: {
            const f32 freq = op.constant.x == 0.0f ? 4.0f : op.constant.x;
            const f32 phase = op.constant.z;
            const f32 x = genCoord().x * freq + phase;
            f32 v;
            switch (static_cast<int>(op.constant.y)) {
                case 1: v = 1.0f - std::fabs(2.0f * Frac(x) - 1.0f); break;      // triangle
                case 2: v = Frac(x); break;                                     // saw
                case 3: v = Frac(x) < 0.5f ? 1.0f : 0.0f; break;                // square
                default: v = 0.5f + 0.5f * std::sin(x * 6.2831853f); break;     // sine
            }
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::Dots: {
            const f32 t = op.constant.x == 0.0f ? 4.0f : op.constant.x;
            const f32 r = op.constant.y == 0.0f ? 0.3f : op.constant.y;
            const glm::vec2 c = glm::vec2(Frac(genCoord().x * t), Frac(genCoord().y * t)) - 0.5f;
            const f32 v = 1.0f - glm::smoothstep(r - 0.03f, r, glm::length(c));
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::RadialGradient: {
            const f32 r = op.constant.x == 0.0f ? 0.5f : op.constant.x;
            const f32 v = std::clamp(1.0f - glm::length(genCoord() - 0.5f) / r, 0.0f, 1.0f);
            return glm::vec4(v, v, v, 1.0f);
        }
        case NodeType::AngularGradient: {
            const glm::vec2 q = genCoord() - 0.5f;
            const f32 v = std::atan2(q.y, q.x) / 6.2831853f + 0.5f;
            return glm::vec4(v, v, v, 1.0f);
        }

        // === Filters ===
        case NodeType::Blend: {
            const glm::vec4 a = has[0] ? in[0] : glm::vec4(0.0f);
            const glm::vec4 b = has[1] ? in[1] : glm::vec4(0.0f);
            const f32 op2 = op.constant.y == 0.0f ? 1.0f : op.constant.y;
            const glm::vec3 blended = BlendMode(static_cast<int>(op.constant.x), glm::vec3(a), glm::vec3(b));
            return glm::vec4(glm::mix(glm::vec3(a), blended, std::clamp(op2, 0.0f, 1.0f)), a.a);
        }
        case NodeType::HSV: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            glm::vec3 hsv = Rgb2Hsv(glm::vec3(c));
            hsv.x = Frac(hsv.x + op.constant.x);
            hsv.y = std::clamp(hsv.y * (op.constant.y == 0.0f ? 1.0f : op.constant.y), 0.0f, 1.0f);
            hsv.z = std::clamp(hsv.z * (op.constant.z == 0.0f ? 1.0f : op.constant.z), 0.0f, 4.0f);
            return glm::vec4(Hsv2Rgb(hsv), c.a);
        }
        case NodeType::BrightnessContrast: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 con = op.constant.y == 0.0f ? 1.0f : op.constant.y;
            glm::vec3 r = (glm::vec3(c) - 0.5f) * con + 0.5f + op.constant.x;
            return glm::vec4(glm::clamp(r, 0.0f, 1.0f), c.a);
        }
        case NodeType::Levels: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 inLo = op.constant.x, inHi = op.constant.y == 0.0f ? 1.0f : op.constant.y;
            const f32 outLo = op.constant.z, outHi = op.constant.w == 0.0f ? 1.0f : op.constant.w;
            const f32 span = (inHi - inLo);
            glm::vec3 r;
            for (int i = 0; i < 3; ++i) {
                const f32 t = std::abs(span) < 1e-6f ? 0.0f : std::clamp((c[i] - inLo) / span, 0.0f, 1.0f);
                r[i] = outLo + t * (outHi - outLo);
            }
            return glm::vec4(r, c.a);
        }
        case NodeType::Gamma: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 g = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            glm::vec3 r;
            for (int i = 0; i < 3; ++i) r[i] = std::pow(std::max(c[i], 0.0f), g);
            return glm::vec4(r, c.a);
        }
        case NodeType::Posterize: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 lv = op.constant.x < 2.0f ? 4.0f : op.constant.x;
            glm::vec3 r;
            for (int i = 0; i < 3; ++i) r[i] = std::floor(c[i] * lv) / (lv - 1.0f);
            return glm::vec4(glm::clamp(r, 0.0f, 1.0f), c.a);
        }
        case NodeType::Threshold: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 t = op.constant.x == 0.0f ? 0.5f : op.constant.x;
            const f32 lum = glm::dot(glm::vec3(c), glm::vec3(0.299f, 0.587f, 0.114f));
            const f32 v = lum >= t ? 1.0f : 0.0f;
            return glm::vec4(v, v, v, c.a);
        }
        case NodeType::Grayscale: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            const f32 v = glm::dot(glm::vec3(c), glm::vec3(0.299f, 0.587f, 0.114f));
            return glm::vec4(v, v, v, c.a);
        }
        case NodeType::Combine:
            return glm::vec4(has[0] ? in[0].x : 0.0f, has[1] ? in[1].x : 0.0f,
                             has[2] ? in[2].x : 0.0f, has[3] ? in[3].x : 1.0f);
        case NodeType::Swizzle: {
            const glm::vec4 c = has[0] ? in[0] : glm::vec4(0.0f);
            switch (static_cast<int>(op.constant.x)) {
                case 1: return glm::vec4(c.y);
                case 2: return glm::vec4(c.z);
                case 3: return glm::vec4(c.w);
                case 4: return glm::vec4(glm::dot(glm::vec3(c), glm::vec3(0.299f, 0.587f, 0.114f)));
                default: return glm::vec4(c.x);
            }
        }

        // === SDF value ops ===
        case NodeType::SdfCircle: {
            const f32 r = op.constant.x == 0.0f ? 0.3f : op.constant.x;
            return glm::vec4(glm::length(genCoord() - 0.5f) - r);
        }
        case NodeType::SdfBox: {
            const glm::vec2 h(op.constant.x == 0.0f ? 0.3f : op.constant.x,
                              op.constant.y == 0.0f ? 0.2f : op.constant.y);
            const glm::vec2 p = glm::abs(genCoord() - 0.5f) - h;
            const f32 d = glm::length(glm::max(p, glm::vec2(0.0f))) + std::min(std::max(p.x, p.y), 0.0f);
            return glm::vec4(d);
        }
        case NodeType::SdfOp: {
            const f32 a = has[0] ? in[0].x : 1e9f;
            const f32 b = has[1] ? in[1].x : 1e9f;
            const f32 k = op.constant.y;
            f32 d;
            switch (static_cast<int>(op.constant.x)) {
                case 1: d = std::max(a, -b); break;         // subtract
                case 2: d = std::max(a, b); break;          // intersect
                case 3: d = Smin(a, b, k); break;           // smooth union
                default: d = std::min(a, b); break;         // union
            }
            return glm::vec4(d);
        }
        case NodeType::SdfShow: {
            const f32 d = has[0] ? in[0].x : 1e9f;
            const f32 w = op.constant.x == 0.0f ? 0.01f : op.constant.x;
            const f32 v = 1.0f - glm::smoothstep(0.0f, w, d);
            return glm::vec4(v, v, v, 1.0f);
        }

        case NodeType::Output: return glm::vec4(0.0f); // not a value node
        default: return glm::vec4(0.0f);
    }
}

// Translate(x,y) / rotate(z turns, about 0.5) / scale(w) applied to a UV before it samples input.
glm::vec2 TransformUV(glm::vec2 uv, glm::vec4 c) {
    const f32 scale = c.w == 0.0f ? 1.0f : c.w;
    const glm::vec2 p = (uv - 0.5f) / scale;
    const f32 ang = c.z * 6.2831853f;
    const f32 ca = std::cos(ang), sa = std::sin(ang);
    return glm::vec2(p.x * ca - p.y * sa, p.x * sa + p.y * ca) + 0.5f - glm::vec2(c.x, c.y);
}

// Recursive coordinate-aware evaluation. VALUE nodes evaluate their inputs at the SAME uv; the
// coordinate-transform + resampling nodes re-evaluate their input at a MODIFIED uv - the resamplable-
// function model that makes Transform/Tile/Mirror/Warp/Kaleidoscope + Blur/HeightToNormal/AO/Emboss
// possible at all. Folded constant subtrees return in O(1) and never recurse. Depth-guarded.
glm::vec4 EvalRec(const CompiledGraph& g, int opIdx, const SampleContext& ctx,
                  const TextureProvider& tex, int depth) {
    if (opIdx < 0 || opIdx >= static_cast<int>(g.ops.size()) || depth > 96) return glm::vec4(0.0f);
    const Op& op = g.ops[static_cast<usize>(opIdx)];
    if (op.folded) return op.foldedValue;
    auto inAt = [&](int k, const SampleContext& c, glm::vec4 def) -> glm::vec4 {
        if (op.inputReg[k] == Op::kNoReg) return def;
        return EvalRec(g, static_cast<int>(op.inputReg[k]), c, tex, depth + 1);
    };
    switch (op.type) {
        case NodeType::Transform: {
            SampleContext c = ctx;
            c.uv0 = TransformUV(ctx.uv0, op.constant);
            return inAt(0, c, glm::vec4(0.0f));
        }
        case NodeType::Tile: {
            SampleContext c = ctx;
            const f32 tx = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const f32 ty = op.constant.y == 0.0f ? 1.0f : op.constant.y;
            c.uv0 = glm::vec2(Frac(ctx.uv0.x * tx), Frac(ctx.uv0.y * ty));
            return inAt(0, c, glm::vec4(0.0f));
        }
        case NodeType::Mirror: {
            SampleContext c = ctx;
            auto mir = [](f32 v) { v = std::fmod(v, 2.0f); if (v < 0) v += 2.0f; return v > 1.0f ? 2.0f - v : v; };
            const int m = static_cast<int>(op.constant.x);
            if (m == 0 || m == 2) c.uv0.x = mir(ctx.uv0.x);
            if (m == 1 || m == 2) c.uv0.y = mir(ctx.uv0.y);
            return inAt(0, c, glm::vec4(0.0f));
        }
        case NodeType::Warp: {
            const glm::vec4 off = inAt(1, ctx, glm::vec4(0.5f));
            const f32 amt = op.constant.x == 0.0f ? 0.1f : op.constant.x;
            SampleContext c = ctx;
            c.uv0 += (glm::vec2(off) - 0.5f) * 2.0f * amt;
            return inAt(0, c, glm::vec4(0.0f));
        }
        case NodeType::Kaleidoscope: {
            const f32 n = std::max(1.0f, std::floor(op.constant.x == 0.0f ? 6.0f : op.constant.x));
            const glm::vec2 p = ctx.uv0 - 0.5f;
            f32 a = std::atan2(p.y, p.x);
            const f32 r = glm::length(p);
            const f32 seg = 6.2831853f / n;
            a = std::fabs(std::fmod(a, seg));
            a = std::min(a, seg - a);
            SampleContext c = ctx;
            c.uv0 = glm::vec2(std::cos(a), std::sin(a)) * r + 0.5f;
            return inAt(0, c, glm::vec4(0.0f));
        }
        case NodeType::HeightToNormal: {
            const f32 strength = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const f32 eps = op.constant.y > 0.0f ? op.constant.y : (1.0f / 256.0f);
            auto h = [&](f32 du, f32 dv) { SampleContext c = ctx; c.uv0 += glm::vec2(du, dv); return inAt(0, c, glm::vec4(0.0f)).x; };
            const f32 dhx = (h(eps, 0.0f) - h(-eps, 0.0f)) * strength / (2.0f * eps);
            const f32 dhy = (h(0.0f, eps) - h(0.0f, -eps)) * strength / (2.0f * eps);
            const glm::vec3 nrm = glm::normalize(glm::vec3(-dhx, -dhy, 1.0f));
            return glm::vec4(nrm, 1.0f);
        }
        case NodeType::AmbientOcclusion: {
            const f32 strength = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const f32 rad = op.constant.y > 0.0f ? op.constant.y : (4.0f / 256.0f);
            const f32 h0 = inAt(0, ctx, glm::vec4(0.0f)).x;
            f32 occ = 0.0f;
            int taps = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    SampleContext c = ctx;
                    c.uv0 += glm::vec2(dx, dy) * rad;
                    occ += std::max(0.0f, inAt(0, c, glm::vec4(0.0f)).x - h0);
                    ++taps;
                }
            const f32 ao = 1.0f - std::clamp(occ / static_cast<f32>(taps) * strength * 4.0f, 0.0f, 1.0f);
            return glm::vec4(ao, ao, ao, 1.0f);
        }
        case NodeType::Blur: {
            const f32 rad = op.constant.x > 0.0f ? op.constant.x : (2.0f / 256.0f);
            glm::vec4 sum(0.0f);
            f32 wsum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    SampleContext c = ctx;
                    c.uv0 += glm::vec2(dx, dy) * rad;
                    const f32 w = (dx == 0 && dy == 0) ? 2.0f : 1.0f;
                    sum += inAt(0, c, glm::vec4(0.0f)) * w;
                    wsum += w;
                }
            return sum / wsum;
        }
        case NodeType::Emboss: {
            const f32 strength = op.constant.x == 0.0f ? 1.0f : op.constant.x;
            const f32 eps = 1.0f / 256.0f;
            SampleContext ca = ctx;
            ca.uv0 += glm::vec2(-eps, -eps);
            SampleContext cb = ctx;
            cb.uv0 += glm::vec2(eps, eps);
            const f32 d = (inAt(0, cb, glm::vec4(0.0f)).x - inAt(0, ca, glm::vec4(0.0f)).x) * strength;
            const f32 v = std::clamp(0.5f + d, 0.0f, 1.0f);
            return glm::vec4(v, v, v, 1.0f);
        }
        default: {
            glm::vec4 in[Op::kMaxInputs]{};
            bool has[Op::kMaxInputs] = {false, false, false, false};
            for (int k = 0; k < Op::kMaxInputs; ++k)
                if (op.inputReg[k] != Op::kNoReg) {
                    has[k] = true;
                    in[k] = EvalRec(g, static_cast<int>(op.inputReg[k]), ctx, tex, depth + 1);
                }
            return EvalOpValue(op, in, has, ctx, tex);
        }
    }
}

} // namespace

// ---- Compile ----------------------------------------------------------------------------
CompiledGraph Compile(const Graph& gIn, const std::vector<ParamOverride>& overrides) {
    CompiledGraph out;
    // Apply instance overrides to a copy of the params (never mutate the source graph).
    out.resolvedParams = gIn.params;
    ApplyOverrides(out.resolvedParams, overrides);

    const Node* outputNode = gIn.OutputNode();
    if (!outputNode) {
        out.error = "graph has no (single) Output node";
        return out;
    }

    // Index nodes by id.
    std::unordered_map<u32, const Node*> byId;
    byId.reserve(gIn.nodes.size() * 2);
    for (const auto& n : gIn.nodes) byId[n.id] = &n;

    // Reachability: BFS backward from the Output node's connected inputs (drop dead nodes).
    std::unordered_map<u32, bool> reachable;
    std::vector<u32> stack;
    auto pushInputs = [&](const Node& n) {
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount; ++pin)
            if (const Link* l = gIn.LinkInto(n.id, pin))
                if (byId.count(l->fromNode) && !reachable.count(l->fromNode)) {
                    reachable[l->fromNode] = true;
                    stack.push_back(l->fromNode);
                }
    };
    pushInputs(*outputNode);
    while (!stack.empty()) {
        const u32 id = stack.back();
        stack.pop_back();
        pushInputs(*byId[id]);
    }

    // Stable topological order (Kahn) over the reachable set, iterating nodes in vector order so
    // the result is deterministic regardless of link insertion order.
    std::unordered_map<u32, int> indeg;
    for (const auto& n : gIn.nodes) {
        if (!reachable.count(n.id)) continue;
        int d = 0;
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount; ++pin)
            if (const Link* l = gIn.LinkInto(n.id, pin))
                if (reachable.count(l->fromNode)) ++d;
        indeg[n.id] = d;
    }
    std::vector<u32> order;
    order.reserve(reachable.size());
    bool progress = true;
    std::unordered_map<u32, bool> emitted;
    while (progress) {
        progress = false;
        for (const auto& n : gIn.nodes) { // vector order == deterministic tie-break
            if (!reachable.count(n.id) || emitted.count(n.id)) continue;
            if (indeg[n.id] != 0) continue;
            order.push_back(n.id);
            emitted[n.id] = true;
            progress = true;
            // Decrement consumers.
            for (const auto& m : gIn.nodes) {
                if (!reachable.count(m.id) || emitted.count(m.id)) continue;
                const u8 inCount = NodeInfoOf(m.type).inputCount;
                for (u8 pin = 0; pin < inCount; ++pin)
                    if (const Link* l = gIn.LinkInto(m.id, pin))
                        if (l->fromNode == n.id) indeg[m.id]--;
            }
        }
    }
    if (order.size() != reachable.size()) {
        out.error = "graph contains a cycle";
        return out;
    }

    // Emit ops in topo order; map nodeId -> register index.
    std::unordered_map<u32, u16> reg;
    reg.reserve(order.size() * 2);
    out.ops.reserve(order.size());
    for (const u32 id : order) {
        const Node& n = *byId[id];
        Op op;
        op.type = n.type;
        op.space = n.space;
        op.constant = n.constant;
        op.texture = n.texture;
        op.ramp = n.ramp;
        std::sort(op.ramp.begin(), op.ramp.end(),
                  [](const RampStop& a, const RampStop& b) { return a.pos < b.pos; });
        // If a Constant-family node is bound to an exposed param, bake the resolved value in.
        if ((n.type == NodeType::Constant || n.type == NodeType::Color || n.type == NodeType::Float ||
             n.type == NodeType::Vector) &&
            !n.paramName.empty()) {
            if (const Param* p = out.resolvedParams.Find(n.paramName)) op.constant = p->value;
        }
        const u8 inCount = NodeInfoOf(n.type).inputCount;
        for (u8 pin = 0; pin < inCount && pin < Op::kMaxInputs; ++pin) {
            if (const Link* l = gIn.LinkInto(id, pin)) {
                auto it = reg.find(l->fromNode);
                if (it != reg.end()) op.inputReg[pin] = it->second;
            }
        }
        reg[id] = static_cast<u16>(out.ops.size());
        out.ops.push_back(std::move(op));
    }

    // Constant-fold forward pass. An op folds when its node type is not context/texture dependent
    // and every connected input is itself folded. Unconnected inputs use neutral defaults (const).
    for (usize i = 0; i < out.ops.size(); ++i) {
        Op& op = out.ops[i];
        const bool ctxDep = NodeInfoOf(op.type).contextDependent;
        bool inputsConst = true;
        glm::vec4 in[Op::kMaxInputs]{};
        bool has[Op::kMaxInputs] = {false, false, false, false};
        for (int k = 0; k < Op::kMaxInputs; ++k) {
            if (op.inputReg[k] == Op::kNoReg) continue;
            has[k] = true;
            const Op& src = out.ops[op.inputReg[k]];
            if (!src.folded) {
                inputsConst = false;
            } else {
                in[k] = src.foldedValue;
            }
        }
        if (!ctxDep && inputsConst) {
            SampleContext neutral; // context-independent by construction here
            op.foldedValue = EvalOpValue(op, in, has, neutral, {});
            op.folded = true;
        }
    }

    // Bind Output channels to registers.
    bool allBoundConst = true;
    for (u8 c = 0; c < kChannelCount; ++c) {
        if (const Link* l = gIn.LinkInto(outputNode->id, c)) {
            auto it = reg.find(l->fromNode);
            if (it != reg.end()) {
                out.channelReg[c] = static_cast<int>(it->second);
                if (!out.ops[it->second].folded) allBoundConst = false;
            }
        }
    }
    out.fullyConstant = allBoundConst;
    out.ok = true;
    return out;
}

// ---- Eval -------------------------------------------------------------------------------
SurfaceSample CompiledGraph::Eval(const SampleContext& ctx, const TextureProvider& tex) const {
    SurfaceSample s;
    if (!ok) return s;
    // Evaluate each bound channel by recursively evaluating the op feeding it (coordinate-aware).
    auto ch = [&](Channel c) -> glm::vec4 {
        const int r = channelReg[static_cast<u32>(c)];
        return r >= 0 ? EvalRec(*this, r, ctx, tex, 0) : glm::vec4(0.0f);
    };
    if (channelReg[static_cast<u32>(Channel::BaseColor)] >= 0) s.baseColor = glm::vec3(ch(Channel::BaseColor));
    if (channelReg[static_cast<u32>(Channel::Roughness)] >= 0) s.roughness = ch(Channel::Roughness).x;
    if (channelReg[static_cast<u32>(Channel::Metallic)] >= 0) s.metallic = ch(Channel::Metallic).x;
    if (channelReg[static_cast<u32>(Channel::Normal)] >= 0) {
        const glm::vec3 n = glm::vec3(ch(Channel::Normal));
        s.normalTS = glm::length(n) > 1e-5f ? glm::normalize(n) : glm::vec3(0, 0, 1);
    }
    if (channelReg[static_cast<u32>(Channel::Height)] >= 0) s.height = ch(Channel::Height).x;
    if (channelReg[static_cast<u32>(Channel::AO)] >= 0) s.ao = ch(Channel::AO).x;
    if (channelReg[static_cast<u32>(Channel::Emissive)] >= 0) s.emissive = glm::vec3(ch(Channel::Emissive));
    if (channelReg[static_cast<u32>(Channel::Opacity)] >= 0) s.opacity = ch(Channel::Opacity).x;
    return s;
}

SurfaceParams CompiledGraph::ToSurfaceParams() const {
    SurfaceParams p; // OpenPBR spec defaults
    const SurfaceSample s = Eval(SampleContext{}, {});
    p.base_color = glm::vec4(s.baseColor, s.opacity);
    p.specular_roughness = std::clamp(s.roughness, 0.0f, 1.0f);
    p.base_metalness = std::clamp(s.metallic, 0.0f, 1.0f);
    p.emission_color = s.emissive;
    return p;
}

u64 CompiledGraph::Hash() const {
    u64 h = 1469598103934665603ull;
    h = HashBytes(&fullyConstant, sizeof(fullyConstant), h);
    for (const Op& op : ops) {
        const u8 t = static_cast<u8>(op.type);
        const u8 sp = static_cast<u8>(op.space);
        h = HashBytes(&t, 1, h);
        h = HashBytes(&sp, 1, h);
        for (int k = 0; k < 4; ++k) h = HashF32(op.constant[k], h);
        h = HashBytes(op.inputReg, sizeof(op.inputReg), h);
        h = HashBytes(&op.folded, 1, h);
        for (int k = 0; k < 4; ++k) h = HashF32(op.foldedValue[k], h);
        h = HashStr(op.texture, h);
        for (const RampStop& r : op.ramp) {
            h = HashF32(r.pos, h);
            for (int k = 0; k < 4; ++k) h = HashF32(r.color[k], h);
        }
    }
    for (u32 c = 0; c < kChannelCount; ++c) h = HashBytes(&channelReg[c], sizeof(int), h);
    return h;
}

} // namespace hbe::mat
