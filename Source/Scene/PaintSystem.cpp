// Scene/PaintSystem.cpp - see PaintSystem.h.
#include "Scene/PaintSystem.h"

#include "Assets/AssetLoader.h"
#include "Assets/Mesh.h"
#include "Assets/UAF.h"
#include "Assets/VFS.h"
#include "Core/BinaryStream.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hbe::paint {

namespace {
constexpr char kMagic[4] = {'H', 'P', 'N', 'T'};
constexpr u8 kNeutralHeight = 128; // 0.5 in UNORM (no relief)

inline u8 ToByte(f32 v) {
    return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Cheap deterministic hash noise for procedural brush tips (bristle/chalk/spray).
inline f32 Hash2(i32 x, i32 y) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}
inline f32 ValueNoise(f32 x, f32 y) {
    const i32 xi = static_cast<i32>(std::floor(x)), yi = static_cast<i32>(std::floor(y));
    const f32 fx = x - xi, fy = y - yi;
    const f32 sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = Hash2(xi, yi), b = Hash2(xi + 1, yi);
    const f32 c = Hash2(xi, yi + 1), d = Hash2(xi + 1, yi + 1);
    return glm::mix(glm::mix(a, b, sx), glm::mix(c, d, sx), sy);
}
} // namespace

f32 BrushTip::Sample(f32 u, f32 v) const {
    const i32 x0 = static_cast<i32>(u), y0 = static_cast<i32>(v);
    const i32 x1 = std::min<i32>(x0 + 1, size - 1), y1 = std::min<i32>(y0 + 1, size - 1);
    const f32 fx = u - x0, fy = v - y0;
    const f32 a = alpha[static_cast<usize>(y0) * size + x0];
    const f32 b = alpha[static_cast<usize>(y0) * size + x1];
    const f32 c = alpha[static_cast<usize>(y1) * size + x0];
    const f32 d = alpha[static_cast<usize>(y1) * size + x1];
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

f32 BrushTip::SampleDetail(f32 u, f32 v) const {
    if (detail.size() != static_cast<usize>(size) * size) return 0.5f; // flat
    const i32 x0 = static_cast<i32>(u), y0 = static_cast<i32>(v);
    const i32 x1 = std::min<i32>(x0 + 1, size - 1), y1 = std::min<i32>(y0 + 1, size - 1);
    const f32 fx = u - x0, fy = v - y0;
    const f32 a = detail[static_cast<usize>(y0) * size + x0];
    const f32 b = detail[static_cast<usize>(y0) * size + x1];
    const f32 c = detail[static_cast<usize>(y1) * size + x0];
    const f32 d = detail[static_cast<usize>(y1) * size + x1];
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

BrushTip MakeBrushTip(const BrushDef& d, u32 size) {
    if (size < 8) size = 8;
    BrushTip t;
    t.size = size;
    t.alpha.assign(static_cast<usize>(size) * size, 0.0f);
    t.detail.assign(static_cast<usize>(size) * size, 0.5f); // 0.5 = flat (no micro-relief)

    // Custom tip (imported image / hand-painted pixels): bilinear-resample the
    // stored grayscale stamp to the tip resolution.
    if (d.HasCustom()) {
        const i32 cs = static_cast<i32>(d.customSize);
        const auto at = [&](i32 xx, i32 yy) {
            return d.customAlpha[static_cast<usize>(yy) * cs + xx] / 255.0f;
        };
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                const f32 fx = (x + 0.5f) / size * cs - 0.5f;
                const f32 fy = (y + 0.5f) / size * cs - 0.5f;
                const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), 0, cs - 1);
                const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), 0, cs - 1);
                const i32 x1 = std::min(x0 + 1, cs - 1), y1 = std::min(y0 + 1, cs - 1);
                const f32 tx = std::clamp(fx - x0, 0.0f, 1.0f), ty = std::clamp(fy - y0, 0.0f, 1.0f);
                t.alpha[static_cast<usize>(y) * size + x] =
                    glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx),
                             glm::mix(at(x0, y1), at(x1, y1), tx), ty);
            }
        }
        return t;
    }

    const f32 c = (size - 1) * 0.5f;
    const f32 hardness = std::clamp(d.hardness, 0.0f, 1.0f);
    const f32 inner = hardness * 0.55f;            // smaller solid core -> more feather
    const f32 softPow = glm::mix(3.6f, 1.3f, hardness); // gentler, feathered edges
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 nx = (x - c) / c; // -1..1
            const f32 ny = (y - c) / c;
            const f32 u01 = nx * 0.5f + 0.5f, v01 = ny * 0.5f + 0.5f;
            // Base falloff (round, or a flat ellipse thin along y).
            const f32 ey = (d.shape == 1) ? (ny / 0.34f) : ny;
            const f32 r = std::sqrt(nx * nx + ey * ey);
            if (r > 1.0f) continue;
            f32 a = (r <= inner) ? 1.0f
                                 : std::clamp(1.0f - (r - inner) / std::max(1.0f - inner, 1e-3f),
                                              0.0f, 1.0f);
            a = std::pow(a, softPow);
            // Bristle: the strong expression (impasto ridges + dragged value streaks)
            // is carried by the detail channel; coverage is only broken GENTLY so a
            // loaded brush still reads opaque (dry-brush scratchiness comes from its
            // low flow + grain). Streaks run along the stroke (fine across, long along).
            f32 det = 0.5f;
            const f32 bAmt = std::clamp(d.bristles, 0.0f, 1.0f);
            if (bAmt > 0.0f) {
                const f32 streak = ValueNoise(u01 * 22.0f, v01 * 3.5f);
                det += (streak - 0.5f) * bAmt;                         // bristle micro-relief
                a *= glm::mix(1.0f, 0.55f + 0.45f * streak, bAmt * 0.6f); // gentle coverage break
            }
            // Chalky grain: coverage holes + a little micro-relief grain.
            const f32 gAmt = std::clamp(d.grain, 0.0f, 1.0f);
            if (gAmt > 0.0f) {
                const f32 g = ValueNoise(u01 * 24.0f, v01 * 24.0f);
                a *= glm::mix(1.0f, g < 0.45f ? g * 0.4f : g, gAmt);
                const f32 gd = ValueNoise(u01 * 38.0f, v01 * 38.0f);
                det += (gd - 0.5f) * gAmt * 0.5f;
            }
            // Spray/scatter holes.
            if (d.scatter > 0.0f) {
                const f32 dots = ValueNoise(u01 * 28.0f, v01 * 28.0f);
                if (dots < 0.45f + d.scatter * 0.35f) a *= (1.0f - std::clamp(d.scatter, 0.0f, 1.0f));
            }
            const usize ti = static_cast<usize>(y) * size + x;
            t.alpha[ti] = std::clamp(a, 0.0f, 1.0f);
            t.detail[ti] = std::clamp(det, 0.0f, 1.0f);
        }
    }
    return t;
}

std::vector<BrushDef> DefaultBrushes() {
    std::vector<BrushDef> b;
    //          name        shape hard grain bris scat flow  spacing size  relief
    b.push_back({"Soft",      0, 0.00f, 0.0f, 0.0f, 0.0f, 0.35f, 0.10f, 0.35f, 0.12f});
    b.push_back({"Hard",      0, 0.85f, 0.0f, 0.0f, 0.0f, 0.85f, 0.09f, 0.30f, 0.35f});
    b.push_back({"Bristle",   0, 0.20f, 0.0f, 0.85f, 0.0f, 0.6f, 0.07f, 0.40f, 0.25f});
    b.push_back({"Chalk",     0, 0.25f, 0.65f, 0.0f, 0.0f, 0.55f, 0.09f, 0.45f, 0.40f});
    b.push_back({"Flat",      1, 0.50f, 0.0f, 0.0f, 0.0f, 0.65f, 0.07f, 0.45f, 0.25f});
    // Loaded oil flat: firm leading edge, bristle texture, heavy impasto - the
    // alla-prima "confident stroke" brush. Pair with some Colour variation.
    b.push_back({"Oil Flat",  1, 0.70f, 0.0f, 0.55f, 0.0f, 0.85f, 0.05f, 0.50f, 0.5f});
    b.push_back({"Spray",     0, 0.00f, 0.0f, 0.0f, 0.7f, 0.3f, 0.13f, 0.50f, 0.08f});

    // --- The artist's painting kit (name shape hard grain bris scat flow spacing
    //     size relief mode colorVar) -------------------------------------------
    // Flat paint: solid opaque coverage, firm round edge - blocking in colour.
    b.push_back({"Flat Paint",    0, 0.90f, 0.00f, 0.00f, 0.0f, 0.95f, 0.10f, 0.40f, 0.10f, 0, 0.06f});
    // Dry brush: broken, scratchy bristle with low build-up - catches the tooth.
    b.push_back({"Dry Brush",     0, 0.40f, 0.35f, 0.90f, 0.0f, 0.30f, 0.06f, 0.40f, 0.20f, 0, 0.10f});
    // Palette knife: hard flat smear, heavy impasto relief - confident slabs.
    b.push_back({"Palette Knife", 1, 0.95f, 0.00f, 0.20f, 0.0f, 0.90f, 0.05f, 0.50f, 0.60f, 0, 0.08f});
    // Wash: very soft, low-flow translucent glaze - builds up gradually.
    b.push_back({"Wash",          0, 0.00f, 0.00f, 0.00f, 0.0f, 0.12f, 0.10f, 0.45f, 0.04f, 0, 0.04f});
    // Edge darkening: glazes a darker tone into the stroke edges (recess/pooling).
    b.push_back({"Edge Darken",   0, 0.30f, 0.00f, 0.00f, 0.0f, 0.40f, 0.10f, 0.35f, 0.05f, 2, 0.00f});
    // Colour variation: a normal stroke with strong per-dab hue/value pooling.
    b.push_back({"Colour Var",    0, 0.50f, 0.00f, 0.30f, 0.0f, 0.50f, 0.08f, 0.40f, 0.15f, 0, 1.00f});
    // Smudge: picks up existing paint and drags it along the stroke (blending).
    b.push_back({"Smudge",        0, 0.20f, 0.00f, 0.00f, 0.0f, 0.60f, 0.08f, 0.35f, 0.00f, 1, 0.00f});
    return b;
}

namespace {
// Sizes a layer's buffers for `resolution`: colour transparent (0), material
// neutral (metal 0, rough 0.5, height 0.5, coverage 0).
void SizeLayer(PaintLayer& l, u32 resolution) {
    const usize n = static_cast<usize>(resolution) * resolution * 4;
    l.color.assign(n, 0);
    l.material.assign(n, 0);
    for (usize i = 0; i < n; i += 4) {
        l.material[i + 1] = 128; // roughness 0.5
        l.material[i + 2] = kNeutralHeight; // height 0.5
    }
}
} // namespace

int AddLayer(PaintComponent& p, const std::string& name) {
    PaintLayer l;
    l.name = name;
    SizeLayer(l, p.resolution < 16 ? 1024 : p.resolution);
    p.layers.push_back(std::move(l));
    p.dirty = true;
    return static_cast<int>(p.layers.size()) - 1;
}

void EnsureCanvas(PaintComponent& p, u32 resolution) {
    if (resolution < 16) resolution = 16;
    const usize n = static_cast<usize>(resolution) * resolution * 4;
    const bool resChanged = p.resolution != resolution;
    p.resolution = resolution;
    if (resChanged) {
        p.colorTex = {};
        p.matTex = {};
        p.gpuReady = false;
    }
    // Guarantee at least one layer, and (re)size every layer to the resolution.
    if (p.layers.empty()) {
        PaintLayer base;
        base.name = "Base";
        p.layers.push_back(std::move(base));
    }
    for (PaintLayer& l : p.layers)
        if (l.color.size() != n || l.material.size() != n) SizeLayer(l, resolution);
    p.activeLayer = std::clamp(p.activeLayer, 0, static_cast<int>(p.layers.size()) - 1);
    p.dirty = true;
}

bool RaycastMesh(const MeshData& mesh, const glm::vec3& ro, const glm::vec3& rd,
                 PaintHit& out) {
    const std::vector<Vertex>& V = mesh.vertices;
    const std::vector<u32>& I = mesh.indices;
    f32 best = 1e30f;
    bool hit = false;
    for (usize i = 0; i + 2 < I.size(); i += 3) {
        const Vertex& v0 = V[I[i]];
        const Vertex& v1 = V[I[i + 1]];
        const Vertex& v2 = V[I[i + 2]];
        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        const glm::vec3 pv = glm::cross(rd, e2);
        const f32 det = glm::dot(e1, pv);
        if (std::fabs(det) < 1e-9f) continue; // ray parallel to the triangle
        const f32 inv = 1.0f / det;
        const glm::vec3 tv = ro - v0.position;
        const f32 u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 qv = glm::cross(tv, e1);
        const f32 w = glm::dot(rd, qv) * inv;
        if (w < 0.0f || u + w > 1.0f) continue;
        const f32 t = glm::dot(e2, qv) * inv;
        if (t < 1e-4f || t >= best) continue;

        best = t;
        hit = true;
        const f32 b0 = 1.0f - u - w; // barycentric for v0,v1,v2
        out.t = t;
        out.localPos = ro + rd * t;
        out.uv = v0.uv * b0 + v1.uv * u + v2.uv * w;
        const glm::vec3 gn = glm::cross(e1, e2);
        out.localNormal = (glm::dot(gn, gn) > 1e-20f) ? glm::normalize(gn)
                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
        // UV density: sqrt(uvTriArea / worldTriArea) = UV units per world unit.
        const glm::vec2 d1 = v1.uv - v0.uv;
        const glm::vec2 d2 = v2.uv - v0.uv;
        const f32 worldArea = glm::length(glm::cross(e1, e2));        // 2*area
        const f32 uvArea = std::fabs(d1.x * d2.y - d1.y * d2.x);      // 2*uv area
        out.uvPerWorld = (worldArea > 1e-9f) ? std::sqrt(uvArea / worldArea) : 1.0f;
    }
    return hit;
}

MeshData BuildRibbon(const std::vector<glm::vec3>& pts,
                     const std::vector<glm::vec3>& normals, f32 width,
                     bool doubleSided) {
    MeshData m;
    m.name = "Stroke";
    const int n = static_cast<int>(pts.size());
    if (n < 2 || static_cast<int>(normals.size()) != n) return m;

    // Cumulative arc length (for tiling UV.u along the stroke).
    std::vector<f32> len(n, 0.0f);
    for (int i = 1; i < n; ++i) len[i] = len[i - 1] + glm::distance(pts[i], pts[i - 1]);
    const f32 wTile = std::max(width * 2.0f, 1e-3f);

    m.vertices.reserve(static_cast<usize>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const glm::vec3 P = pts[i];
        glm::vec3 N = glm::normalize(normals[i]);
        // Path direction projected onto the surface plane -> across-stroke axis.
        glm::vec3 T = (i == 0) ? (pts[1] - pts[0])
                     : (i == n - 1) ? (pts[n - 1] - pts[n - 2])
                                    : (pts[i + 1] - pts[i - 1]);
        T = T - N * glm::dot(T, N);
        if (glm::dot(T, T) < 1e-10f) T = glm::abs(N.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        T = glm::normalize(T);
        const glm::vec3 B = glm::normalize(glm::cross(N, T));
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n - 1);
        // Taper the ends for a brush-stroke look (exaggerated, pointed-ish).
        const f32 taper = glm::smoothstep(0.0f, 0.14f, t) * glm::smoothstep(0.0f, 0.14f, 1.0f - t);
        const f32 w = width * std::max(taper, 0.1f);
        const f32 u = len[i] / wTile;
        const glm::vec3 lift = N * 0.02f;
        auto vert = [&](const glm::vec3& pos, f32 vv) {
            Vertex v;
            v.position = pos;
            v.normal = N;
            v.tangent = glm::vec4(T, 1.0f);
            v.uv = glm::vec2(u, vv);
            return v;
        };
        m.vertices.push_back(vert(P + B * w + lift, 0.0f));
        m.vertices.push_back(vert(P - B * w + lift, 1.0f));
    }
    for (int i = 0; i + 1 < n; ++i) {
        const u32 a = static_cast<u32>(i * 2);
        m.indices.insert(m.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
        // Back faces (reversed winding) so a free-floating stroke is visible from
        // EITHER side when you orbit around it (grease-pencil strokes are 2-sided).
        if (doubleSided)
            m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    return m;
}

MeshData BuildRibbon(const std::vector<glm::vec3>& pts,
                     const std::vector<glm::vec3>& normals,
                     const std::vector<f32>& halfWidths,
                     bool doubleSided, bool arcLengthUV) {
    MeshData m;
    m.name = "Stroke";
    const int n = static_cast<int>(pts.size());
    if (n < 2 || static_cast<int>(normals.size()) != n ||
        static_cast<int>(halfWidths.size()) != n)
        return m;

    std::vector<f32> len(n, 0.0f);
    f32 wmax = 1e-4f;
    for (int i = 1; i < n; ++i) len[i] = len[i - 1] + glm::distance(pts[i], pts[i - 1]);
    for (int i = 0; i < n; ++i) wmax = std::max(wmax, halfWidths[i]);
    const f32 total = std::max(len[n - 1], 1e-4f);
    const f32 wTile = std::max(wmax * 2.0f, 1e-3f); // tiling fallback

    m.vertices.reserve(static_cast<usize>(n) * 2);
    for (int i = 0; i < n; ++i) {
        const glm::vec3 P = pts[i];
        glm::vec3 N = glm::normalize(normals[i]);
        glm::vec3 T = (i == 0) ? (pts[1] - pts[0])
                     : (i == n - 1) ? (pts[n - 1] - pts[n - 2])
                                    : (pts[i + 1] - pts[i - 1]);
        T = T - N * glm::dot(T, N);
        if (glm::dot(T, T) < 1e-10f) T = glm::abs(N.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        T = glm::normalize(T);
        const glm::vec3 B = glm::normalize(glm::cross(N, T));
        const f32 w = std::max(halfWidths[i], 0.0f); // taper/jitter already applied
        const f32 u = arcLengthUV ? (len[i] / total) : (len[i] / wTile);
        const glm::vec3 lift = N * 0.02f;
        auto vert = [&](const glm::vec3& pos, f32 vv) {
            Vertex v;
            v.position = pos;
            v.normal = N;
            v.tangent = glm::vec4(T, 1.0f);
            v.uv = glm::vec2(u, vv);
            return v;
        };
        m.vertices.push_back(vert(P + B * w + lift, 0.0f));
        m.vertices.push_back(vert(P - B * w + lift, 1.0f));
    }
    for (int i = 0; i + 1 < n; ++i) {
        const u32 a = static_cast<u32>(i * 2);
        m.indices.insert(m.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
        if (doubleSided)
            m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    return m;
}

BoxParams ComputeBoxParams(const glm::vec3& localMin, const glm::vec3& localMax,
                           const glm::vec3& worldScale) {
    BoxParams b;
    b.center = (localMin + localMax) * 0.5f;
    b.scale = worldScale;
    const glm::vec3 worldExt = (localMax - localMin) * glm::abs(worldScale);
    const f32 m = std::max({worldExt.x, worldExt.y, worldExt.z, 1e-4f});
    b.invM = 1.0f / m;
    return b;
}

// Must match BoxPaintUV() in MeshPBR.hlsl. Picks the dominant face axis from the
// normal, projects the in-plane world-scaled offset, and packs into a 4x4 atlas
// cell (6 of 16 cells used). Uniform world density -> no stretch under scaling.
glm::vec2 BoxProjectUV(const glm::vec3& localPos, const glm::vec3& localNormal,
                       const BoxParams& b) {
    const glm::vec3 wc = (localPos - b.center) * b.scale; // world-centered offset
    const glm::vec3 an = glm::abs(localNormal);
    glm::vec2 plane;
    int col = 0, row = 0;
    if (an.x >= an.y && an.x >= an.z) {
        plane = {wc.z, wc.y};
        col = (localNormal.x >= 0.0f) ? 0 : 1;
        row = 0;
    } else if (an.y >= an.z) {
        plane = {wc.x, wc.z};
        col = (localNormal.y >= 0.0f) ? 2 : 3;
        row = 0;
    } else {
        plane = {wc.x, wc.y};
        col = (localNormal.z >= 0.0f) ? 0 : 1;
        row = 1;
    }
    glm::vec2 cell = plane * b.invM + 0.5f;
    cell = glm::clamp(cell, 0.0f, 1.0f);
    return (glm::vec2(static_cast<f32>(col), static_cast<f32>(row)) + cell) * 0.25f;
}

namespace {
// Writes one dab texel into the colour (`c`) + material (`m`) bytes. `a` is the tip
// coverage at this texel (tip alpha * flow already applied); `detail` is the brush's
// bristle/grain micro-relief at this texel (0.5 neutral). The micro-relief drags a
// light/dark VALUE streak through the pigment and lays IMPASTO ridges into the relief
// height, so a mark reads as loaded oil paint, not a flat decal. Shared by the UV-disc
// Stamp and the 3D StampProjected so the two paint identically. (Smudge needs a
// neighbour lookup, so Stamp keeps that mode inline and never reaches here for it.)
inline void WriteDabTexel(u8* c, u8* m, f32 a, f32 detail, f32 flow, f32 brushCov,
                          const glm::vec4& dabColor, const Dab& dab) {
    // Bristle drag: a +/- value streak in the laid pigment, centred on neutral so a
    // flat fill keeps its colour on average (no bristle -> detail 0.5 -> no streak).
    const f32 streak = (detail - 0.5f) * 0.55f; // +/- ~0.27 value
    glm::vec3 pig(std::clamp(dabColor.r * (1.0f + streak), 0.0f, 1.0f),
                  std::clamp(dabColor.g * (1.0f + streak), 0.0f, 1.0f),
                  std::clamp(dabColor.b * (1.0f + streak), 0.0f, 1.0f));

    if (dab.mode == static_cast<int>(BrushMode::EdgeDarken)) {
        // Darken the existing paint toward the (dark) brush colour, weighted by the
        // tip's falloff band so darkness pools at stroke edges/recesses.
        const f32 edge = a * (1.0f - a) * 4.0f; // peaks in the soft edge band
        const f32 w = edge * flow * 0.8f;
        c[0] = ToByte((c[0] / 255.0f) * (1.0f - w) + pig.r * w * 0.35f);
        c[1] = ToByte((c[1] / 255.0f) * (1.0f - w) + pig.g * w * 0.35f);
        c[2] = ToByte((c[2] / 255.0f) * (1.0f - w) + pig.b * w * 0.35f);
        return;
    }
    if (dab.erase) {
        const f32 keep = 1.0f - a;
        for (int k = 0; k < 4; ++k) c[k] = static_cast<u8>(c[k] * keep);
        // Relax material toward neutral + drop its coverage.
        m[0] = static_cast<u8>(m[0] * keep);
        m[1] = ToByte(m[1] / 255.0f + (0.5f - m[1] / 255.0f) * a);
        m[2] = ToByte(m[2] / 255.0f + (0.5f - m[2] / 255.0f) * a);
        m[3] = static_cast<u8>(m[3] * keep);
        return;
    }
    if (dab.paintColor) {
        // Straight-alpha "over" of the pigment; coverage gated by brush alpha.
        const f32 da = c[3] / 255.0f;
        const f32 av = a * brushCov;
        const f32 outA = av + da * (1.0f - av);
        const auto over = [&](f32 s, f32 d) {
            return outA > 1e-5f ? (s * av + d * da * (1.0f - av)) / outA : 0.0f;
        };
        c[0] = ToByte(over(pig.r, c[0] / 255.0f));
        c[1] = ToByte(over(pig.g, c[1] / 255.0f));
        c[2] = ToByte(over(pig.b, c[2] / 255.0f));
        c[3] = ToByte(outA);
    }
    if (dab.paintMaterial) {
        // Converge metal/rough toward the brush values; build coverage.
        m[0] = ToByte(m[0] / 255.0f + (std::clamp(dab.metallic, 0.0f, 1.0f) - m[0] / 255.0f) * a);
        m[1] = ToByte(m[1] / 255.0f + (std::clamp(dab.roughness, 0.0f, 1.0f) - m[1] / 255.0f) * a);
        // Impasto relief: paint piles up where laid (additive, capped) with bristle
        // GROOVES so the ridges rake light like real thick oil paint. The grooves
        // (detail-driven) dominate the smooth buildup so the surface reads as bristled.
        const f32 buildup = dab.height * a;
        const f32 grooves = (detail - 0.5f) * dab.height * 4.0f * a;
        m[2] = ToByte(m[2] / 255.0f + buildup + grooves); // relief (0.5 neutral)
        m[3] = ToByte(a + (m[3] / 255.0f) * (1.0f - a));
    }
}

// Deterministic per-dab colour pooling, hashed from a stable seed (the dab UV or
// 3D centre) so live painting and a stroke-database rebake produce identical pixels.
// Mutates `dabColor` in place when colorVar is active. The shifts are deliberately
// bold (oil-painter "broken colour": a white wall picks up clear creams/blues/greys).
inline void ApplyColorVar(glm::vec4& dabColor, const Dab& dab, f32 seed) {
    if (dab.colorVar <= 0.0f || !dab.paintColor) return;
    const auto hash = [](f32 n) { return std::fmod(std::abs(std::sin(n) * 43758.5453f), 1.0f); };
    const f32 a1 = hash(seed) - 0.5f;          // warm <-> cool
    const f32 a2 = hash(seed + 11.1f) - 0.5f;  // value
    const f32 a3 = hash(seed + 23.7f) - 0.5f;  // saturation-ish
    const f32 amt = dab.colorVar;
    dabColor.r = std::clamp(dabColor.r + (a1 * 0.14f + a2 * 0.11f) * amt, 0.0f, 1.0f);
    dabColor.g = std::clamp(dabColor.g + (a3 * 0.08f + a2 * 0.11f) * amt, 0.0f, 1.0f);
    dabColor.b = std::clamp(dabColor.b + (-a1 * 0.14f + a2 * 0.11f) * amt, 0.0f, 1.0f);
}

// A stable reference tangent on a surface plane (any unit vector perpendicular to
// `n`). StampProjected and SurfaceAngle MUST derive the tip frame from the same
// construction so the live stroke and a rebake orient directional tips identically.
inline glm::vec3 SurfaceTangent(const glm::vec3& n) {
    return (std::fabs(n.y) < 0.9f) ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), n))
                                   : glm::normalize(glm::cross(glm::vec3(1, 0, 0), n));
}
} // namespace

void Stamp(PaintComponent& p, int layerIndex, const glm::vec2& uvIn, f32 uvRadius,
           const BrushTip& tip, f32 angleRad, const Dab& dab) {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(p.layers.size()) || !tip.Valid()) return;
    PaintLayer& L = p.layers[layerIndex];
    if (L.color.empty() || L.material.empty()) return;
    const i32 res = static_cast<i32>(p.resolution);
    const f32 cx = uvIn.x * res;
    const f32 cy = uvIn.y * res;
    const f32 rpx = std::max(uvRadius * res, 0.75f);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor(cx - rpx)), 0, res - 1);
    const i32 x1 = std::clamp(static_cast<i32>(std::ceil(cx + rpx)), 0, res - 1);
    const i32 y0 = std::clamp(static_cast<i32>(std::floor(cy - rpx)), 0, res - 1);
    const i32 y1 = std::clamp(static_cast<i32>(std::ceil(cy + rpx)), 0, res - 1);
    const f32 flow = std::clamp(dab.flow, 0.0f, 1.0f);
    const f32 ca = std::cos(-angleRad), sa = std::sin(-angleRad);
    const f32 tmax = static_cast<f32>(tip.size - 1);

    // Per-dab colour variation: a small warm/cool + value/saturation shift so a
    // "flat" colour pools into subtle painterly variety (a white wall picks up
    // faint blues/creams/greys). Hashed from the dab UV (deterministic).
    glm::vec4 dabColor = dab.color;
    ApplyColorVar(dabColor, dab, uvIn.x * 311.7f + uvIn.y * 197.3f + 0.13f);
    const f32 brushCov = std::clamp(dabColor.a, 0.0f, 1.0f);
    const f32 sdx = std::cos(angleRad), sdy = std::sin(angleRad); // stroke direction

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 dx = (x + 0.5f) - cx;
            const f32 dy = (y + 0.5f) - cy;
            const f32 rx = dx * ca - dy * sa, ry = dx * sa + dy * ca; // into tip space
            const f32 tu = (rx / rpx * 0.5f + 0.5f) * tmax;
            const f32 tv = (ry / rpx * 0.5f + 0.5f) * tmax;
            if (tu < 0.0f || tu > tmax || tv < 0.0f || tv > tmax) continue;
            const f32 a = tip.Sample(tu, tv) * flow;
            if (a <= 0.0f) continue;
            const f32 detail = tip.SampleDetail(tu, tv); // bristle micro-relief

            const usize idx = (static_cast<usize>(y) * res + x) * 4;
            u8* c = &L.color[idx];
            u8* m = &L.material[idx];
            if (dab.mode == static_cast<int>(BrushMode::Smudge)) {
                // Pick up colour from a texel BEHIND the stroke and pull it forward,
                // blended by the tip alpha - drags/blends existing paint.
                const f32 pick = rpx * 0.5f;
                const i32 sx = std::clamp(static_cast<i32>(std::lround(x - sdx * pick)), 0, res - 1);
                const i32 sy = std::clamp(static_cast<i32>(std::lround(y - sdy * pick)), 0, res - 1);
                const u8* src = &L.color[(static_cast<usize>(sy) * res + sx) * 4];
                const f32 bl = a * 0.7f;
                for (int k = 0; k < 4; ++k)
                    c[k] = ToByte(c[k] / 255.0f + (src[k] / 255.0f - c[k] / 255.0f) * bl);
                continue;
            }
            WriteDabTexel(c, m, a, detail, flow, brushCov, dabColor, dab);
        }
    }
    p.dirty = true;
}

void StampProjected(PaintComponent& p, int layerIndex, const MeshData& mesh,
                    const glm::vec3& center, const glm::vec3& normal, f32 localRadius,
                    const BrushTip& tip, f32 angleRad, const Dab& dab) {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(p.layers.size()) || !tip.Valid()) return;
    if (localRadius <= 0.0f) return;
    PaintLayer& L = p.layers[layerIndex];
    if (L.color.empty() || L.material.empty()) return;
    const i32 res = static_cast<i32>(p.resolution);
    const f32 flow = std::clamp(dab.flow, 0.0f, 1.0f);
    const f32 r2 = localRadius * localRadius;

    // Per-dab colour pooling, deterministic from the 3D dab centre (so live painting
    // and a rebake match), then the brush coverage ceiling.
    glm::vec4 dabColor = dab.color;
    ApplyColorVar(dabColor, dab, center.x * 311.7f + center.y * 197.3f + center.z * 131.1f + 0.13f);
    const f32 brushCov = std::clamp(dabColor.a, 0.0f, 1.0f);

    // Tangent frame on the surface so the brush TIP keeps its shape/orientation when
    // projected (flat/bristle tips align to the stroke via angleRad).
    glm::vec3 n = (glm::dot(normal, normal) > 1e-12f) ? glm::normalize(normal) : glm::vec3(0, 1, 0);
    const glm::vec3 t = SurfaceTangent(n);
    const glm::vec3 b = glm::cross(n, t);
    const f32 ca = std::cos(angleRad), sa = std::sin(angleRad);
    const glm::vec3 ax = t * ca + b * sa; // rotated tip U axis
    const glm::vec3 ay = -t * sa + b * ca; // rotated tip V axis
    const f32 tmax = static_cast<f32>(tip.size - 1);
    const f32 invRes = 1.0f / static_cast<f32>(res);

    const std::vector<Vertex>& V = mesh.vertices;
    const std::vector<u32>& I = mesh.indices;
    for (usize i = 0; i + 2 < I.size(); i += 3) {
        const Vertex& v0 = V[I[i]];
        const Vertex& v1 = V[I[i + 1]];
        const Vertex& v2 = V[I[i + 2]];
        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        glm::vec3 fn = glm::cross(e1, e2);
        const f32 fnl2 = glm::dot(fn, fn);
        if (fnl2 < 1e-20f) continue;
        fn *= 1.0f / std::sqrt(fnl2);
        if (glm::dot(fn, n) < 0.3f) continue; // only paint roughly co-facing surface

        // Reject triangles whose local AABB is entirely outside the brush sphere.
        const glm::vec3 tmin = glm::min(v0.position, glm::min(v1.position, v2.position));
        const glm::vec3 tmaxp = glm::max(v0.position, glm::max(v1.position, v2.position));
        const glm::vec3 cl = glm::clamp(center, tmin, tmaxp);
        if (glm::dot(cl - center, cl - center) > r2) continue;

        // UV-space barycentric basis (rasterize this triangle into the canvas).
        const glm::vec2 a0 = v0.uv, d1 = v1.uv - v0.uv, d2 = v2.uv - v0.uv;
        const f32 den = d1.x * d2.y - d1.y * d2.x;
        if (std::fabs(den) < 1e-12f) continue; // degenerate UVs
        const f32 invDen = 1.0f / den;
        const glm::vec2 uvmin = glm::min(v0.uv, glm::min(v1.uv, v2.uv));
        const glm::vec2 uvmax = glm::max(v0.uv, glm::max(v1.uv, v2.uv));
        const i32 x0 = std::clamp(static_cast<i32>(std::floor(uvmin.x * res)), 0, res - 1);
        const i32 x1 = std::clamp(static_cast<i32>(std::ceil(uvmax.x * res)), 0, res - 1);
        const i32 y0 = std::clamp(static_cast<i32>(std::floor(uvmin.y * res)), 0, res - 1);
        const i32 y1 = std::clamp(static_cast<i32>(std::ceil(uvmax.y * res)), 0, res - 1);

        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const glm::vec2 puv((x + 0.5f) * invRes, (y + 0.5f) * invRes);
                const glm::vec2 dp = puv - a0;
                const f32 w1 = (dp.x * d2.y - dp.y * d2.x) * invDen;
                const f32 w2 = (d1.x * dp.y - d1.y * dp.x) * invDen;
                const f32 w0 = 1.0f - w1 - w2;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue; // outside tri
                const glm::vec3 lp = v0.position * w0 + v1.position * w1 + v2.position * w2;
                const glm::vec3 off = lp - center;
                if (glm::dot(off, off) > r2) continue; // outside brush sphere

                // Project the surface offset onto the tip's tangent plane -> tip uv.
                const f32 pu = glm::dot(off, ax), pv = glm::dot(off, ay);
                const f32 tu = (pu / localRadius * 0.5f + 0.5f) * tmax;
                const f32 tvv = (pv / localRadius * 0.5f + 0.5f) * tmax;
                if (tu < 0.0f || tu > tmax || tvv < 0.0f || tvv > tmax) continue;
                const f32 av = tip.Sample(tu, tvv) * flow;
                if (av <= 0.0f) continue;
                const f32 detail = tip.SampleDetail(tu, tvv); // bristle micro-relief

                const usize idx = (static_cast<usize>(y) * res + x) * 4;
                WriteDabTexel(&L.color[idx], &L.material[idx], av, detail, flow, brushCov, dabColor, dab);
            }
        }
    }
    p.dirty = true;
}

f32 SurfaceAngle(const glm::vec3& normal, const glm::vec3& dir) {
    const glm::vec3 n = (glm::dot(normal, normal) > 1e-12f) ? glm::normalize(normal)
                                                            : glm::vec3(0, 1, 0);
    glm::vec3 d = dir - n * glm::dot(dir, n); // project onto the surface plane
    if (glm::dot(d, d) < 1e-12f) return 0.0f;
    const glm::vec3 t = SurfaceTangent(n);
    const glm::vec3 b = glm::cross(n, t);
    return std::atan2(glm::dot(d, b), glm::dot(d, t));
}

void FillLayer(PaintComponent& p, int layerIndex, const Dab& dab, bool clear) {
    if (layerIndex < 0 || layerIndex >= static_cast<int>(p.layers.size())) return;
    PaintLayer& L = p.layers[layerIndex];
    if (L.color.empty() || L.material.empty()) return;
    const u8 r = ToByte(dab.color.r), g = ToByte(dab.color.g), b = ToByte(dab.color.b);
    const u8 ca = clear ? 0 : ToByte(dab.color.a);
    const u8 mm = ToByte(dab.metallic), mr = ToByte(dab.roughness);
    const u8 ma = clear ? 0 : (dab.paintMaterial ? ToByte(dab.color.a) : 0);
    for (usize i = 0; i < L.color.size(); i += 4) {
        L.color[i] = r; L.color[i + 1] = g; L.color[i + 2] = b;
        L.color[i + 3] = dab.paintColor ? ca : 0;
        L.material[i] = mm; L.material[i + 1] = mr;
        L.material[i + 2] = kNeutralHeight; L.material[i + 3] = ma;
    }
    p.dirty = true;
}

namespace {
// Two procedural brushes bake the same tip (so a rebake can reuse it). Custom-tip
// brushes are never pooled (cheap to be safe).
bool SameTip(const BrushDef& a, const BrushDef& b) {
    if (a.HasCustom() || b.HasCustom()) return false;
    return a.shape == b.shape && a.hardness == b.hardness && a.grain == b.grain &&
           a.bristles == b.bristles && a.scatter == b.scatter;
}
} // namespace

// Replays one recorded stroke into its layer - the inverse of recording it while
// painting. Path strokes interpolate dabs between consecutive points at the brush
// spacing, matching the editor's live stamping so a rebake is pixel-identical.
// `mesh` is needed only for projection (mode-2) strokes; null skips them.
void ReplayStroke(PaintComponent& p, const Stroke& s, const BrushTip& tip,
                  const MeshData* mesh) {
    if (s.layer < 0 || s.layer >= static_cast<int>(p.layers.size())) return;
    PaintLayer& L = p.layers[s.layer];
    if (s.type == StrokeType::Clear) {
        std::fill(L.color.begin(), L.color.end(), static_cast<u8>(0));
        std::fill(L.material.begin(), L.material.end(), static_cast<u8>(0));
        p.dirty = true;
        return;
    }

    Dab dab;
    dab.color = s.color;
    dab.metallic = s.metallic;
    dab.roughness = s.roughness;
    dab.height = s.height;
    dab.flow = s.flow;
    dab.colorVar = s.colorVar;
    dab.paintColor = s.paintColor;
    dab.paintMaterial = s.paintMaterial;
    dab.erase = s.erase;
    dab.mode = s.brush.mode;

    if (s.type == StrokeType::Fill) { FillLayer(p, s.layer, dab, false); return; }
    if (!tip.Valid() || s.path.empty()) return;

    // 3D projection strokes interpolate dab centres along the LOCAL-space path and
    // stamp by surface proximity (crosses UV seams; needs the mesh geometry).
    if (s.projection == 2) {
        if (!mesh) return; // can't reproduce a projection stroke without geometry
        const auto dabAt = [&](const glm::vec3& c, const glm::vec3& nrm, f32 radius,
                               f32 pressure, f32 angle) {
            Dab d = dab;
            d.flow = s.flow * pressure;
            StampProjected(p, s.layer, *mesh, c, nrm, radius, tip, angle, d);
        };
        f32 ang0 = 0.0f;
        if (s.path.size() > 1)
            ang0 = SurfaceAngle(s.path[0].localNormal, s.path[1].localPos - s.path[0].localPos);
        dabAt(s.path[0].localPos, s.path[0].localNormal, s.path[0].localRadius,
              s.path[0].pressure, ang0);
        for (usize i = 1; i < s.path.size(); ++i) {
            const StrokePoint& a = s.path[i - 1];
            const StrokePoint& b = s.path[i];
            const glm::vec3 d = b.localPos - a.localPos;
            const f32 dist = glm::length(d);
            const f32 angle = SurfaceAngle(b.localNormal, d);
            const f32 spacing = std::max(b.localRadius * s.brush.spacing, 1e-5f);
            const int steps = std::min(static_cast<int>(dist / spacing), 512);
            for (int k = 1; k <= steps; ++k) {
                const f32 t = static_cast<f32>(k) / static_cast<f32>(steps + 1);
                dabAt(glm::mix(a.localPos, b.localPos, t),
                      glm::mix(a.localNormal, b.localNormal, t),
                      glm::mix(a.localRadius, b.localRadius, t),
                      glm::mix(a.pressure, b.pressure, t), angle);
            }
            dabAt(b.localPos, b.localNormal, b.localRadius, b.pressure, angle);
        }
        return;
    }

    const f32 texel = 1.0f / std::max(static_cast<f32>(p.resolution), 1.0f);
    const auto dabAt = [&](const glm::vec2& uv, f32 radius, f32 pressure, f32 angle) {
        Dab d = dab;
        d.flow = s.flow * pressure;
        Stamp(p, s.layer, uv, radius, tip, angle, d);
    };

    // First point (angle toward the next point).
    f32 ang0 = 0.0f;
    if (s.path.size() > 1) {
        const glm::vec2 d0 = s.path[1].uv - s.path[0].uv;
        if (glm::dot(d0, d0) > 1e-10f) ang0 = std::atan2(d0.y, d0.x);
    }
    dabAt(s.path[0].uv, s.path[0].radius, s.path[0].pressure, ang0);

    for (usize i = 1; i < s.path.size(); ++i) {
        const StrokePoint& a = s.path[i - 1];
        const StrokePoint& b = s.path[i];
        const glm::vec2 d = b.uv - a.uv;
        const f32 dist = glm::length(d);
        const f32 angle = (glm::dot(d, d) > 1e-10f) ? std::atan2(d.y, d.x) : 0.0f;
        if (dist > 0.25f) { dabAt(b.uv, b.radius, b.pressure, angle); continue; } // UV-island jump
        const f32 spacing = std::max(b.radius * s.brush.spacing, 0.5f * texel);
        const int steps = std::min(static_cast<int>(dist / spacing), 512);
        for (int k = 1; k <= steps; ++k) {
            const f32 t = static_cast<f32>(k) / static_cast<f32>(steps + 1);
            dabAt(a.uv + d * t, glm::mix(a.radius, b.radius, t),
                  glm::mix(a.pressure, b.pressure, t), angle);
        }
        dabAt(b.uv, b.radius, b.pressure, angle);
    }
}

void BakeFromStrokes(PaintComponent& p, const MeshData* mesh) {
    for (PaintLayer& L : p.layers) {
        std::fill(L.color.begin(), L.color.end(), static_cast<u8>(0));
        std::fill(L.material.begin(), L.material.end(), static_cast<u8>(0));
    }
    BrushTip tip;
    bool haveTip = false;
    const BrushDef* lastBrush = nullptr;
    for (const Stroke& s : p.strokes) {
        if (s.type == StrokeType::Path &&
            (!haveTip || lastBrush == nullptr || !SameTip(*lastBrush, s.brush))) {
            tip = MakeBrushTip(s.brush);
            haveTip = true;
            lastBrush = &s.brush;
        }
        ReplayStroke(p, s, tip, mesh);
    }
    Flatten(p);
    p.dirty = true;
}

void Flatten(PaintComponent& p) {
    const usize n = static_cast<usize>(p.resolution) * p.resolution * 4;
    p.flatColor.assign(n, 0);
    p.flatMaterial.assign(n, 0);
    for (usize i = 0; i < n; i += 4) {
        p.flatMaterial[i + 1] = 128;            // roughness 0.5
        p.flatMaterial[i + 2] = kNeutralHeight; // height 0.5
    }
    for (const PaintLayer& L : p.layers) {
        if (!L.visible || L.color.size() != n || L.material.size() != n) continue;
        const f32 lo = std::clamp(L.opacity, 0.0f, 1.0f);
        for (usize i = 0; i < n; i += 4) {
            // Colour: straight-alpha over.
            const f32 ca = (L.color[i + 3] / 255.0f) * lo;
            if (ca > 0.0f) {
                const f32 da = p.flatColor[i + 3] / 255.0f;
                const f32 outA = ca + da * (1.0f - ca);
                const auto over = [&](u8 s, u8 d) {
                    return outA > 1e-5f ? (s / 255.0f * ca + d / 255.0f * da * (1.0f - ca)) / outA
                                        : 0.0f;
                };
                p.flatColor[i] = ToByte(over(L.color[i], p.flatColor[i]));
                p.flatColor[i + 1] = ToByte(over(L.color[i + 1], p.flatColor[i + 1]));
                p.flatColor[i + 2] = ToByte(over(L.color[i + 2], p.flatColor[i + 2]));
                p.flatColor[i + 3] = ToByte(outA);
            }
            // Material: lerp metal/rough/height toward the layer by its coverage.
            const f32 ma = (L.material[i + 3] / 255.0f) * lo;
            if (ma > 0.0f) {
                const auto mix = [&](u8 s, u8 d) { return ToByte(d / 255.0f + (s / 255.0f - d / 255.0f) * ma); };
                p.flatMaterial[i] = mix(L.material[i], p.flatMaterial[i]);
                p.flatMaterial[i + 1] = mix(L.material[i + 1], p.flatMaterial[i + 1]);
                p.flatMaterial[i + 2] = mix(L.material[i + 2], p.flatMaterial[i + 2]);
                p.flatMaterial[i + 3] = ToByte(ma + (p.flatMaterial[i + 3] / 255.0f) * (1.0f - ma));
            }
        }
    }
}

namespace {
// Pads painted texels outward into the empty (alpha==0) gutter so bilinear filtering
// and mip generation don't bleed background/black across UV-island seams. Fills the
// colour channels (0..2) of empty texels from their nearest painted neighbours,
// leaving alpha at 0 - so the shader's coverage still tapers to nothing at the seam,
// just with the correct hue instead of black. `iterations` = padding width (texels).
// Works for both buffers: channel 3 is the validity/coverage alpha in each. Scans
// only the painted bounding box (+ the growing frontier) so a small stroke is cheap.
void DilateEdges(std::vector<u8>& buf, i32 res, int iterations) {
    if (res <= 0 || iterations <= 0) return;
    const usize n = static_cast<usize>(res) * res;
    if (buf.size() != n * 4) return;

    std::vector<u8> valid(n, 0);
    i32 minX = res, minY = res, maxX = -1, maxY = -1;
    for (i32 y = 0; y < res; ++y) {
        for (i32 x = 0; x < res; ++x) {
            const usize idx = static_cast<usize>(y) * res + x;
            if (buf[idx * 4 + 3] > 0) {
                valid[idx] = 1;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0) return; // nothing painted

    std::vector<u8> next = valid; // accumulates newly-filled texels per ring
    for (int it = 0; it < iterations; ++it) {
        // A newly fillable texel can only sit one ring beyond the current bounds.
        minX = std::max(minX - 1, 0); minY = std::max(minY - 1, 0);
        maxX = std::min(maxX + 1, res - 1); maxY = std::min(maxY + 1, res - 1);
        bool any = false;
        for (i32 y = minY; y <= maxY; ++y) {
            for (i32 x = minX; x <= maxX; ++x) {
                const usize idx = static_cast<usize>(y) * res + x;
                if (valid[idx]) continue;
                i32 r = 0, g = 0, b = 0, cnt = 0;
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const i32 nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
                        const usize nidx = static_cast<usize>(ny) * res + nx;
                        if (!valid[nidx]) continue;
                        r += buf[nidx * 4 + 0]; g += buf[nidx * 4 + 1]; b += buf[nidx * 4 + 2];
                        ++cnt;
                    }
                }
                if (cnt > 0) {
                    buf[idx * 4 + 0] = static_cast<u8>(r / cnt);
                    buf[idx * 4 + 1] = static_cast<u8>(g / cnt);
                    buf[idx * 4 + 2] = static_cast<u8>(b / cnt);
                    // alpha (channel 3) stays 0: coverage still vanishes at the seam.
                    next[idx] = 1;
                    any = true;
                }
            }
        }
        valid = next; // promote the new ring before growing again
        if (!any) break;
    }
}
} // namespace

void Sync(Renderer& renderer, PaintComponent& p, bool dilateEdges) {
    if (!p.dirty) return;
    if (p.layers.empty()) { p.dirty = false; return; }
    Flatten(p);
    if (dilateEdges) {
        constexpr int kEdgePadding = 4; // texels of seam gutter padding
        DilateEdges(p.flatColor, static_cast<i32>(p.resolution), kEdgePadding);
        DilateEdges(p.flatMaterial, static_cast<i32>(p.resolution), kEdgePadding);
    }

    auto upload = [&](const std::vector<u8>& srcPixels, rhi::Format fmt,
                      rhi::TextureHandle& handle) {
        uaf::Texture tex;
        tex.width = p.resolution;
        tex.height = p.resolution;
        tex.format = static_cast<u32>(fmt);
        tex.mipCount = 1;
        tex.pixels = srcPixels; // copy; GenerateMips appends the chain
        assets::GenerateMips(tex);
        rhi::TextureDesc desc;
        desc.width = tex.width;
        desc.height = tex.height;
        desc.format = fmt;
        desc.mipCount = tex.mipCount;
        desc.pixels = tex.pixels.data();
        desc.debugName = "PaintCanvas";
        if (handle.IsValid()) renderer.UpdateTexture(handle, desc);
        else handle = renderer.UploadTexture(desc);
    };

    // Colour as sRGB (pigment averages in linear); material as UNORM.
    upload(p.flatColor, rhi::Format::R8G8B8A8_SRGB, p.colorTex);
    upload(p.flatMaterial, rhi::Format::R8G8B8A8_UNORM, p.matTex);
    p.gpuReady = p.colorTex.IsValid() && p.matTex.IsValid();
    p.dirty = false;
}

namespace {
void WriteBrushDef(BinaryWriter& w, const BrushDef& d) {
    w.Str(d.name);
    w.Pod<i32>(d.shape);
    w.Pod<f32>(d.hardness); w.Pod<f32>(d.grain); w.Pod<f32>(d.bristles); w.Pod<f32>(d.scatter);
    w.Pod<f32>(d.flow); w.Pod<f32>(d.spacing); w.Pod<f32>(d.size); w.Pod<f32>(d.relief);
    w.Pod<i32>(d.mode); w.Pod<f32>(d.colorVar);
    w.Pod<u32>(d.customSize);
    w.Vec(d.customAlpha);
}
void ReadBrushDef(BinaryReader& r, BrushDef& d) {
    r.Str(d.name);
    r.Pod(d.shape);
    r.Pod(d.hardness); r.Pod(d.grain); r.Pod(d.bristles); r.Pod(d.scatter);
    r.Pod(d.flow); r.Pod(d.spacing); r.Pod(d.size); r.Pod(d.relief);
    r.Pod(d.mode); r.Pod(d.colorVar);
    r.Pod(d.customSize);
    r.Vec(d.customAlpha);
}
} // namespace

bool Save(const std::filesystem::path& absPath, const PaintComponent& p) {
    if (p.layers.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(absPath.parent_path(), ec);
    BinaryWriter w;
    w.Bytes(kMagic, 4);
    w.Pod<u32>(4); // v4: v3 + stroke projection mode + per-point local pos/normal/radius
    w.Pod<u32>(p.resolution);
    w.Pod<i32>(p.activeLayer);
    w.Pod<u32>(static_cast<u32>(p.layers.size()));
    for (const PaintLayer& L : p.layers) {
        w.Str(L.name);
        w.Pod<f32>(L.opacity);
        w.Pod<u8>(L.visible ? 1 : 0);
        w.Vec(L.color);
        w.Vec(L.material);
    }
    // Stroke database: the editable source of truth (runtime ignores it; the baked
    // layers above are the fast load path).
    w.Pod<u32>(static_cast<u32>(p.strokes.size()));
    for (const Stroke& s : p.strokes) {
        w.Pod<i32>(static_cast<i32>(s.type));
        w.Pod<i32>(s.layer);
        w.Pod<i32>(s.projection); // v4
        WriteBrushDef(w, s.brush);
        w.Pod<glm::vec4>(s.color);
        w.Pod<f32>(s.metallic); w.Pod<f32>(s.roughness); w.Pod<f32>(s.height);
        w.Pod<f32>(s.flow); w.Pod<f32>(s.colorVar);
        w.Pod<u8>(s.paintColor ? 1 : 0);
        w.Pod<u8>(s.paintMaterial ? 1 : 0);
        w.Pod<u8>(s.erase ? 1 : 0);
        w.Pod<u32>(static_cast<u32>(s.path.size()));
        for (const StrokePoint& pt : s.path) {
            w.Pod<glm::vec2>(pt.uv);
            w.Pod<f32>(pt.radius);
            w.Pod<f32>(pt.pressure);
            w.Pod<glm::vec3>(pt.localPos);     // v4 (projection)
            w.Pod<glm::vec3>(pt.localNormal);  // v4
            w.Pod<f32>(pt.localRadius);        // v4
        }
    }
    return w.SaveToFile(absPath);
}

bool Load(const std::filesystem::path& absPath, PaintComponent& p) {
    std::optional<std::vector<u8>> bytes = vfs::ReadFile(absPath);
    if (!bytes || bytes->empty()) return false;
    BinaryReader r(*bytes);
    char magic[4] = {};
    if (!r.Bytes(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return false;
    u32 version = 0, resolution = 0;
    r.Pod(version);
    r.Pod(resolution);
    if (!r.Ok() || resolution == 0) return false;
    const usize n = static_cast<usize>(resolution) * resolution * 4;

    p.resolution = resolution;
    p.layers.clear();
    p.colorTex = {};
    p.matTex = {};
    p.gpuReady = false;
    p.dirty = true;

    if (version >= 2) {
        r.Pod(p.activeLayer);
        u32 count = 0;
        r.Pod(count);
        for (u32 i = 0; i < count; ++i) {
            PaintLayer L;
            r.Str(L.name);
            r.Pod(L.opacity);
            u8 vis = 1; r.Pod(vis); L.visible = vis != 0;
            r.Vec(L.color);
            r.Vec(L.material);
            if (!r.Ok() || L.color.size() != n || L.material.size() != n) return false;
            p.layers.push_back(std::move(L));
        }
    } else {
        // v1: a single colour + height buffer -> one layer (height -> material.b).
        std::vector<u8> color, height;
        r.Vec(color);
        r.Vec(height);
        if (!r.Ok() || color.size() != n || height.size() != n) return false;
        PaintLayer L;
        L.name = "Base";
        L.color = std::move(color);
        L.material.assign(n, 0);
        for (usize i = 0; i < n; i += 4) {
            L.material[i + 1] = 128;            // roughness 0.5
            L.material[i + 2] = height[i];      // v1 height was in R
            L.material[i + 3] = L.color[i + 3]; // material coverage = colour coverage
        }
        p.layers.push_back(std::move(L));
        p.activeLayer = 0;
    }
    if (p.layers.empty()) p.layers.emplace_back(PaintLayer{});
    p.activeLayer = std::clamp(p.activeLayer, 0, static_cast<int>(p.layers.size()) - 1);

    // v3+: the stroke database (editable history). Older files have none - the baked
    // layers above are still authoritative; the editor will re-record from there on.
    // v4 added stroke.projection + per-point local pos/normal/radius (3D projection).
    p.strokes.clear();
    if (version >= 3) {
        u32 sc = 0;
        r.Pod(sc);
        for (u32 i = 0; i < sc && r.Ok(); ++i) {
            Stroke s;
            i32 ty = 0; r.Pod(ty); s.type = static_cast<StrokeType>(ty);
            r.Pod(s.layer);
            if (version >= 4) r.Pod(s.projection);
            ReadBrushDef(r, s.brush);
            r.Pod(s.color);
            r.Pod(s.metallic); r.Pod(s.roughness); r.Pod(s.height);
            r.Pod(s.flow); r.Pod(s.colorVar);
            u8 pc8 = 1, pm = 1, er = 0;
            r.Pod(pc8); r.Pod(pm); r.Pod(er);
            s.paintColor = pc8 != 0; s.paintMaterial = pm != 0; s.erase = er != 0;
            u32 pn = 0; r.Pod(pn);
            s.path.reserve(pn);
            for (u32 k = 0; k < pn && r.Ok(); ++k) {
                StrokePoint pt;
                r.Pod(pt.uv); r.Pod(pt.radius); r.Pod(pt.pressure);
                if (version >= 4) {
                    r.Pod(pt.localPos); r.Pod(pt.localNormal); r.Pod(pt.localRadius);
                }
                s.path.push_back(pt);
            }
            if (r.Ok()) p.strokes.push_back(std::move(s));
        }
    }
    return true;
}

} // namespace hbe::paint
