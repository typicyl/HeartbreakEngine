// Scene/StrokeDirTest.cpp - see StrokeDirTest.h.
#include "Scene/StrokeDirTest.h"

#include "Editor/MovieRender.h" // WritePng

#include "../../Shaders/ProceduralPaint.hlsli" // the shader, compiled as C++

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hbe::procpaint {
namespace {

constexpr u32 kRes = 700;
constexpr f32 kPi = 3.14159265358979f;

// A surface sample: where the ray hit, and the normal there. `hit` false = miss.
struct Surf {
    bool hit = false;
    glm::vec3 P{0.0f};
    glm::vec3 N{0.0f, 1.0f, 0.0f};
};

// --- the three test surfaces ------------------------------------------------
// Deliberately placed WELL AWAY from the world origin (see the shear analysis in
// ProceduralPaint.hlsli): a formulation that only works near (0,0,0) must fail
// here rather than pass by accident.
constexpr glm::vec3 kFarOffset{137.0f, -62.0f, 211.0f};

Surf PlaneAt(u32 x, u32 y) {
    // A flat plane seen face-on, 8 object units across.
    Surf s;
    s.hit = true;
    const f32 u = (x + 0.5f) / kRes * 8.0f - 4.0f;
    const f32 v = (y + 0.5f) / kRes * 8.0f - 4.0f;
    s.P = kFarOffset + glm::vec3(u, 0.0f, v);
    s.N = glm::vec3(0.0f, 1.0f, 0.0f);
    return s;
}

Surf SphereAt(u32 x, u32 y) {
    // Orthographic ray down -Z onto a sphere of radius 3.2.
    Surf s;
    const f32 u = ((x + 0.5f) / kRes * 2.0f - 1.0f) * 3.6f;
    const f32 v = ((y + 0.5f) / kRes * 2.0f - 1.0f) * 3.6f;
    const f32 r2 = u * u + v * v;
    constexpr f32 R = 3.2f;
    if (r2 > R * R) return s;
    const f32 w = std::sqrt(R * R - r2);
    s.hit = true;
    s.N = glm::normalize(glm::vec3(u, v, w));
    s.P = kFarOffset + s.N * R;
    return s;
}

Surf BoxAt(u32 x, u32 y) {
    // A cube seen down a corner, so three faces with different normals meet in
    // view - the case that exposes a tangent-frame discontinuity.
    Surf s;
    const f32 u = ((x + 0.5f) / kRes * 2.0f - 1.0f) * 4.6f;
    const f32 v = ((y + 0.5f) / kRes * 2.0f - 1.0f) * 4.6f;
    // Isometric-ish projection of a 3-unit cube.
    const f32 c = 0.8660254f, sn = 0.5f;
    const glm::vec3 right(c, 0.0f, -c);
    const glm::vec3 up(-sn * c, 0.8660254f, -sn * c);
    const glm::vec3 dir = glm::normalize(glm::cross(right, up));
    const glm::vec3 ro = (right * u + up * v) - dir * 20.0f;
    constexpr f32 H = 2.6f;
    // Slab test against the axis-aligned cube [-H,H]^3.
    f32 t0 = -1e30f, t1 = 1e30f;
    int axis = 0;
    f32 sign = 1.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 d = dir[a];
        if (std::abs(d) < 1e-6f) {
            if (ro[a] < -H || ro[a] > H) return s;
            continue;
        }
        f32 ta = (-H - ro[a]) / d, tb = (H - ro[a]) / d;
        f32 sg = -1.0f;
        if (ta > tb) { std::swap(ta, tb); sg = 1.0f; }
        if (ta > t0) { t0 = ta; axis = a; sign = sg; }
        t1 = std::min(t1, tb);
        if (t0 > t1) return s;
    }
    if (t0 < 0.0f) return s;
    s.hit = true;
    s.P = kFarOffset + ro + dir * t0;
    s.N = glm::vec3(0.0f);
    s.N[axis] = sign;
    return s;
}

void WriteRGB(const std::filesystem::path& p, const std::vector<glm::vec3>& rgb) {
    std::vector<u8> px(static_cast<usize>(kRes) * kRes * 4);
    for (usize i = 0; i < rgb.size(); ++i) {
        for (int k = 0; k < 3; ++k)
            px[i * 4 + static_cast<usize>(k)] =
                static_cast<u8>(std::clamp(rgb[i][k], 0.0f, 1.0f) * 255.0f + 0.5f);
        px[i * 4 + 3] = 255;
    }
    movie::WritePng(p, kRes, kRes, px);
}

struct DirStats {
    f32 p50Turn = 0.0f, p99Turn = 0.0f;
    f32 flipFrac = 0.0f; // share of neighbour pairs turning > 25 deg
    f32 idRunAlong = 0.0f, idRunAcross = 0.0f;
    f32 occupancy = 0.0f;
};

f32 Percentile(std::vector<f32> v, f32 q) {
    if (v.empty()) return 0.0f;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

bool DirectionSelfTest(const std::filesystem::path& outDir, std::string& report) {
    const bool wantImages = !outDir.empty();
    if (wantImages) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
    }

    ProcPaintParams prm = ProcPaintDefaults();
    // The visualization patch is 8 units across at 700 px, so ~87 px/unit against
    // the oracle patch's 256. Scale the stroke so it still lands at the
    // ground-truth ~24 px wide with a ~20:1 body.
    prm.halfWidth = 0.14f;
    prm.length = 2.5f;       // ~1.4 cells, so consecutive strokes overlap along a lane
    prm.laneSpacing = 0.17f; // < 2*halfWidth: neighbouring lanes overlap across
    const f32 flowScale = 0.055f; // direction wavelength ~18 units: regional, not busy
    // CURVATURE CONSTRAINT. A stroke is a straight band, so it can only lie flat
    // on a surface over a span short relative to the radius of curvature: the
    // frame must turn by L/R radians along it. The first run used L = 6 on a
    // radius-3.2 sphere - about 2 radians - and the sphere pinched and tore.
    // Region size bounds the coordinate origin's distance from the shaded point,
    // so it bounds that shear directly; it has to stay near the stroke length.
    const f32 cellSize = 1.8f; // ~ stroke length: bounds a stroke's reach
    // A cell of area cellSize^2 needs about
    //   overlap * cellSize^2 / (2*halfWidth * 2*halfLength)
    // strokes to cover itself; ~12 at these numbers with a 2.5x overlap.
    const int kStrokesPerCell = 12;

    struct Case {
        const char* name;
        Surf (*at)(u32, u32);
    };
    const Case cases[] = {{"plane", PlaneAt}, {"sphere", SphereAt}, {"box", BoxAt}};

    std::printf("--- stroke arrangement under a VARYING direction field ---\n");
    std::printf("  patch 8.0 units at %ux%u (%.0f px/unit); stroke %.0f px wide, %.0f:1 aspect\n",
                kRes, kRes, kRes / 8.0f, prm.halfWidth * 2.0f * (kRes / 8.0f),
                prm.length / (prm.halfWidth * 2.0f));
    std::printf("  flow wavelength %.0f units, region size %.1f units\n", 1.0f / flowScale,
                cellSize);
    std::printf("  %-8s %-9s %-9s %-8s %-11s %-11s %-6s\n", "surface", "turn p50", "turn p99",
                ">25deg", "id run alng", "id run acrs", "ratio");

    bool ok = true;
    for (const Case& cs : cases) {
        std::vector<glm::vec3> paint(static_cast<usize>(kRes) * kRes, glm::vec3(0.10f));
        std::vector<glm::vec3> dirView(static_cast<usize>(kRes) * kRes, glm::vec3(0.05f));
        std::vector<u32> ids(static_cast<usize>(kRes) * kRes, 0u);
        std::vector<u8> hitMask(static_cast<usize>(kRes) * kRes, 0);
        std::vector<glm::vec3> dirs(static_cast<usize>(kRes) * kRes, glm::vec3(0.0f));

        usize lit = 0, hits = 0;
        for (u32 y = 0; y < kRes; ++y)
            for (u32 x = 0; x < kRes; ++x) {
                const Surf s = cs.at(x, y);
                const usize i = static_cast<usize>(y) * kRes + x;
                if (!s.hit) continue;
                hitMask[i] = 1;
                ++hits;

                const PaintSample ps = ProcPaintStrokes(s.P, s.N, prm, flowScale, cellSize, kStrokesPerCell);
                // Bare BODY over a mid ground - no lighting, no relief, no detail.
                const f32 a = std::clamp(ps.coverage, 0.0f, 1.0f);
                paint[i] = ps.albedo * a + glm::vec3(0.34f) * (1.0f - a);
                ids[i] = ps.dominantId;
                if (a > 0.5f) ++lit;

                // Direction debug view: RGB = the region's tangent direction.
                // The direction of the stroke ACTUALLY SEEN here, carried out
                // of the evaluator - so this view shows the per-stroke frame
                // rather than a field sampled independently of the strokes.
                dirs[i] = ps.dominantDir;
                dirView[i] = ps.dominantDir * 0.5f + 0.5f;
            }

        // --- direction continuity, over surface neighbours only ---------------
        std::vector<f32> turns;
        turns.reserve(hits * 2);
        // MEASURED BETWEEN ADJACENT STROKES, not adjacent pixels. With per-stroke
        // frames the direction is constant inside a stroke and steps at its edge,
        // so a per-pixel measure reports those steps as "flips" and says nothing
        // about the field. The question the success criteria actually ask is
        // whether NEIGHBOURING STROKES point in similar directions.
        //
        // `withinMax` is the other half: the change inside one stroke, which must
        // be exactly zero. That is the no-shear guarantee, worth asserting.
        f32 withinMax = 0.0f;
        for (u32 y = 0; y < kRes; ++y)
            for (u32 x = 0; x + 1 < kRes; ++x) {
                const usize i = static_cast<usize>(y) * kRes + x;
                if (!hitMask[i] || !hitMask[i + 1]) continue;
                // Undirected: a stroke reads the same along D and -D.
                const f32 d = std::min(1.0f, std::abs(glm::dot(dirs[i], dirs[i + 1])));
                const f32 deg = std::acos(d) * 180.0f / kPi;
                if (ids[i] == ids[i + 1]) withinMax = std::max(withinMax, deg);
                else turns.push_back(deg);
            }

        DirStats st;
        st.p50Turn = Percentile(turns, 0.50f);
        st.p99Turn = Percentile(turns, 0.99f);
        usize flips = 0;
        for (f32 t : turns)
            if (t > 25.0f) ++flips;
        st.flipFrac = turns.empty() ? 0.0f : static_cast<f32>(flips) / static_cast<f32>(turns.size());
        st.occupancy = hits ? static_cast<f32>(lit) / static_cast<f32>(hits) : 0.0f;

        // Stroke-id run lengths, measured along and across the LOCAL direction is
        // hard on a curved surface, so measure along both image axes and report
        // the larger/smaller. A body scores a big ratio; hatching scores a big
        // ratio too, so this is necessary but not sufficient - the images decide.
        const auto idRun = [&](bool alongX) {
            usize runs = 0, total = 0;
            for (u32 a = 0; a < kRes; ++a) {
                u32 cur = 0xFFFFFFFFu;
                for (u32 b = 0; b < kRes; ++b) {
                    const usize i = alongX ? static_cast<usize>(a) * kRes + b
                                           : static_cast<usize>(b) * kRes + a;
                    if (!hitMask[i]) { cur = 0xFFFFFFFFu; continue; }
                    if (ids[i] != cur) { ++runs; cur = ids[i]; }
                    ++total;
                }
            }
            return runs ? static_cast<f32>(total) / static_cast<f32>(runs) : 0.0f;
        };
        const f32 ra = idRun(true), rb = idRun(false);
        st.idRunAlong = std::max(ra, rb);
        st.idRunAcross = std::min(ra, rb);

        std::printf("    max direction change WITHIN one stroke: %.4f deg (must be ~0)\n",
                    withinMax);
        std::printf("  %-8s %8.2f%c %8.2f%c %7.2f%% %11.1f %11.1f %6.2f\n", cs.name, st.p50Turn,
                    248, st.p99Turn, 248, st.flipFrac * 100.0f, st.idRunAlong, st.idRunAcross,
                    st.idRunAcross > 0.01f ? st.idRunAlong / st.idRunAcross : 0.0f);

        if (wantImages) {
            WriteRGB(outDir / (std::string("dir_") + cs.name + "_paint.png"), paint);
            WriteRGB(outDir / (std::string("dir_") + cs.name + "_direction.png"), dirView);
            std::vector<glm::vec3> idView(ids.size());
            for (usize i = 0; i < ids.size(); ++i) {
                const u32 h = ids[i] * 2654435761u;
                idView[i] = hitMask[i] ? glm::vec3(static_cast<f32>((h >> 8) & 0xFFu) / 255.0f,
                                                   static_cast<f32>((h >> 14) & 0xFFu) / 255.0f,
                                                   static_cast<f32>((h >> 20) & 0xFFu) / 255.0f)
                                       : glm::vec3(0.05f);
            }
            WriteRGB(outDir / (std::string("dir_") + cs.name + "_strokeid.png"), idView);
        }

        // Gates. Painterly variation is wanted, so this checks for COHERENT
        // variation, not smoothness: a low typical turn with flips confined to a
        // small share of the surface. The box is exempt from the turn gates - a
        // cube's faces genuinely have unrelated tangent planes, and the seam there
        // is geometry, not a basis singularity.
        const bool isBox = std::string(cs.name) == "box";
        // Zero shear INSIDE a stroke is the whole point of the per-stroke frame,
        // so assert it rather than assume it.
        if (withinMax > 0.5f) {  // 0.5 deg: above float noise, far below any real shear
        
            std::printf("    FAIL: direction shears INSIDE strokes on %s\n", cs.name);
            ok = false;
        }
        // Adjacent strokes should differ by a little, not a lot: coherent regional
        // variation rather than per-stroke randomness.
        if (!isBox && st.p50Turn > 22.0f) {
            std::printf("    FAIL: neighbouring strokes disagree too much on %s\n", cs.name);
            ok = false;
        }
        if (!isBox && st.flipFrac > 0.50f) {
            std::printf("    FAIL: too many abrupt stroke-to-stroke jumps on %s\n", cs.name);
            ok = false;
        }
        if (st.occupancy < 0.45f) {
            std::printf("    FAIL: %s is not actually painted (%.0f%% covered)\n", cs.name,
                        st.occupancy * 100.0f);
            ok = false;
        }
    }

    if (wantImages) std::printf("  images -> %s\n", outDir.string().c_str());
    report = ok ? "direction field is coherent and the surfaces are painted"
                : "stroke arrangement under varying direction is wrong";
    return ok;
}

} // namespace hbe::procpaint
